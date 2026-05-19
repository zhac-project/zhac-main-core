// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <atomic>
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "hap_slave.h"
#include "hap_session.h"
#include "hap_json.h"
#include "zhc_adapter.h"
#include "event_bus.h"
#include "zigbee_mgr.h"
#include "zigbee_diagnostics.h"
#include "znp_driver.h"
#include "metrics/metrics.h"
#include "task_stacks.h"
#include "lua_engine.h"
#include "device_shadow.h"
#include "zigbee_pool.h"
#include "zap_store.h"
#include "simple_rules.h"
#include "rule_store.h"
#include "esp_timer.h"
#include "mqtt_gw.h"
#include "tg_gw.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "p4_ota.h"
#include "sdkconfig.h"
#ifdef CONFIG_ZHAC_NCP_EZSP
#include "ezsp_backend.h"
#else
#include "zigbee_backend.h"
#endif

static const char* TAG = "p4_main";

static constexpr uint8_t ZIGBEE_PERMIT_JOIN_STARTUP_S = 250;

// ── Log drain queue ───────────────────────────────────────────────────────
// Captures ESP-IDF log output and forwards it to S3 via HAP LOG_LINE frames.
// Queue entries are fixed 128-byte strings; excess is silently truncated.
// The hook is non-blocking: lines are dropped when the queue is full rather
// than stalling the calling task.
struct LogEntry { char line[HAP_LOG_MSG_MAX]; };
static QueueHandle_t s_log_queue;
// CC-F9: depth bumped 32→128 (≈16 KB at 128 B lines, comfortable on
// P4) and an atomic drop counter exposed so an interview storm or
// panic burst is visible in the heartbeat metrics rather than
// silently swallowed.
static constexpr size_t LOG_QUEUE_DEPTH = 128;
static std::atomic<uint32_t> s_log_dropped{0};

uint32_t p4_log_dropped_total() { return s_log_dropped.load(std::memory_order_relaxed); }

static int log_vprintf_hook(const char* fmt, va_list args) {
    // Duplicate to serial so panics and restart reasons stay visible
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vprintf(fmt, args_copy);
    va_end(args_copy);

    LogEntry e{};
    vsnprintf(e.line, sizeof(e.line), fmt, args);
    if (xQueueSend(s_log_queue, &e, 0) != pdTRUE) {
        // No log here — recursing through the hook itself would amplify
        // the loss. Operators read the counter from heartbeat metrics.
        s_log_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return n;
}

static void task_zigbee(void*);
void task_hap(void*);
static void task_event_bus(void*);
static void task_log_drain(void*);
static void task_wdt(void*);
static void task_stack_mon(void*);
static void task_buttons(void*);

// Forward declaration for send_alert defined in hap_dispatch.cpp
void send_alert(HapAlertCode code, uint64_t ieee, const char* msg);

extern "C" void app_main() {
    esp_reset_reason_t rr = esp_reset_reason();
    static const char* const s_reset_names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT",
        "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO",
    };
    const char* rr_str = (rr < (esp_reset_reason_t)(sizeof(s_reset_names)/sizeof(s_reset_names[0])))
                         ? s_reset_names[rr] : "?";
    ESP_LOGI(TAG, "ZHAC P4 core starting (reset reason: %s)", rr_str);

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms      = 10000,
        .idle_core_mask  = 0,
        .trigger_panic   = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wdt_cfg));

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_ret);
    }

    // Metrics engine — zero-init shard storage. No-op when disabled.
    metrics::init();

    // Lua engine — the project's sole scripting runtime. init() brings
    // up the VM + task. We DON'T call load_all() here — scripts can
    // call into hap_session / tg_gw / rule_store / mqtt_gw etc. as
    // soon as they run, and those subsystems are initialised below.
    // load_all() therefore runs at the very end of app_main once every
    // subsystem the script may touch is up. Until then TaskLua is idle
    // waiting on its resume queue.
    const bool lua_ready = lua_engine_init();

    s_log_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogEntry));
    configASSERT(s_log_queue);

    event_bus_init();
    zap_store_init();
    // Writeback cache: snapshot callback + 1 s tick flush task.
    // Install before any mark_dirty consumer starts (zigbee_mgr_init later
    // wires the pool; cb returns false until the pool is up, which is fine
    // — the flush task will simply skip those entries until the pool
    // fills in).
    zap_store_set_snapshot_cb([](uint64_t ieee, ZapDevice* out) -> bool {
        zigbee_pool_lock();
        const ZapDevice* d = pool_find_by_ieee(ieee);
        bool ok = false;
        if (d) { *out = *d; ok = true; }
        zigbee_pool_unlock();
        return ok;
    });
    zap_store_flush_init();
    esp_register_shutdown_handler(zap_store_flush_now);
#ifdef CONFIG_ZHAC_NCP_EZSP
    ezsp_backend_register();
#else
    zigbee_backend_register();
#endif
    rule_store_init();
    // PSRAM-backed rule writeback cache. User edits land in RAM and
    // batch-commit to NVS every ~5 s (or on graceful shutdown). 32 MB
    // PSRAM makes flash wear the limiting factor, so defer generously.
    rule_store_flush_init();
    esp_register_shutdown_handler(rule_store_flush_now);
    simple_rules_init();
    mqtt_gw_init();
    tg_gw_init();
    hap_slave_init();

    // Subsystems Lua scripts may touch are now up — release the user
    // scripts. Doing this earlier (right after lua_engine_init) raced
    // with hap_session_init/etc. and crashed when a script's first API
    // call hit a NULL mutex.
    if (lua_ready) {
        lua_engine_load_all();
    }

    // Alert producer for rule-level errors. Script errors are now
    // surfaced by the Lua scheduler via METRIC_LUA_ERRORS_TOTAL +
    // ESP_LOGE — no HAP alert for every misbehaving script (they're
    // per-coroutine, not engine-fatal).
    simple_rules_set_error_cb([](uint16_t rule_id, const char* err) {
        char msg[80];
        snprintf(msg, sizeof(msg), "rule %u: %s", rule_id, err);
        send_alert(HapAlertCode::RULE_ERROR, 0, msg);
    });

    // Bring up the ZNP transport. This spawns the RX and worker tasks
    // internally — no separate TaskZNP polling loop is required.
    znp_driver_init();
    // znp_set_wire_trace(true) — opt-in only. Each frame produces 3 INFO
    // logs that forward over HAP as LOG_LINE; at 10 Hz device traffic that
    // saturates the slave TX queue and causes real ALERT/EVT drops.

    // TDD Section 8.1: task priorities and stack sizes
    xTaskCreatePinnedToCore(task_zigbee,    "TaskZigbee",    zhac::stack::kZigbee, nullptr, 5, nullptr, 0);
    // TaskLua is created internally by lua_engine_init()
    xTaskCreatePinnedToCore(task_hap,       "TaskHAP",       zhac::stack::kHapP4, nullptr, 4, nullptr, 0);
    xTaskCreate(             task_event_bus,"TaskEventBus",  zhac::stack::kEventBus, nullptr, 2, nullptr);
    xTaskCreate(             task_log_drain,"TaskLog",       zhac::stack::kLog, nullptr, 1, nullptr);
    xTaskCreatePinnedToCore(task_wdt,       "TaskWDT",       zhac::stack::kWdt, nullptr, 7, nullptr, 0);
    xTaskCreate(             task_stack_mon,"TaskStackMon",  zhac::stack::kStackMonP4, nullptr, 1, nullptr);
    xTaskCreate(             task_buttons,  "TaskButtons",   zhac::stack::kButtons, nullptr, 1, nullptr);

#ifdef CONFIG_HAP_BENCHMARK
    extern void task_hap_bench(void*);
    xTaskCreatePinnedToCore(task_hap_bench, "hap_bench", zhac::stack::kHapBench, nullptr, 3, nullptr, 0);
#endif

    vTaskDelete(nullptr);
}

static void task_zigbee(void*) {
    ESP_LOGI(TAG, "TaskZigbee: starting ZNP init sequence");

    // Bring up the device pool + restore the persisted snapshot BEFORE
    // the shadow consumer so the shadow can iterate the loaded pool
    // directly instead of opening the NVS namespace a second time
    // (03-F06). zap_store_init() ran earlier at boot.
    zigbee_pool_init();
    zigbee_pool_restore_persisted();
    device_shadow_init();
    zigbee_pool_lock();
    device_shadow_restore_from_pool(pool_all(), pool_count());
    zigbee_pool_unlock();
    zb_diag_init();  // unhandled-frame ring (P3 observability, PSRAM)
    zhac_adapter_init();  // parallel observation path for the new ZCL library
    bool ok = zigbee_mgr_init();
    if (!ok) {
        ESP_LOGE(TAG, "ZNP init FAILED — rebooting in 5 s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    ESP_LOGI(TAG, "TaskZigbee: coordinator ready, opening permit join %u s",
             ZIGBEE_PERMIT_JOIN_STARTUP_S);
    zigbee_permit_join(ZIGBEE_PERMIT_JOIN_STARTUP_S);

    // Init done — drop priority so the crash-recovery loop doesn't waste a
    // high-priority slot during normal operation (was 5, now 2).
    vTaskPrioritySet(nullptr, 2);

    // Resolve friendly names now that the device pool is loaded, then fire CTRL_BOOT
    simple_rules_reload();
    Event boot_ev{};
    boot_ev.type = EventType::CTRL_BOOT;
    event_bus_publish(boot_ev);

    // Runtime crash recovery: retry zigbee_mgr_reinit() forever with backoff.
    // coordinator_start() inside reinit already toggles NRESET 5×; we add the
    // outer loop so transient EMI / loose-wire glitches don't cost us the
    // device pool, shadow cache, MQTT session and OTA progress that a P4
    // reboot would discard. Backoff schedule: 3 s, 6 s, 9 s, 12 s, then cap
    // at 60 s — slow enough to surface a real hardware fault in the log
    // without spinning the UART at full duty.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!zigbee_mgr_crashed()) continue;

        ESP_LOGW(TAG, "ZNP crash detected — entering recovery loop");
        uint8_t attempt = 0;
        while (!zigbee_mgr_reinit()) {
            attempt++;
            const uint32_t backoff_ms = (attempt < 5)
                                        ? (3000U * attempt)
                                        : 60000U;
            ESP_LOGE(TAG, "ZNP reinit FAILED (attempt %u) — retry in %u s",
                     attempt, (unsigned)(backoff_ms / 1000));
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }
        ESP_LOGI(TAG, "ZNP recovery OK after %u attempt(s) — reopening permit join %u s",
                 (unsigned)(attempt + 1), ZIGBEE_PERMIT_JOIN_STARTUP_S);
        zigbee_permit_join(ZIGBEE_PERMIT_JOIN_STARTUP_S);
    }
}

static void task_event_bus(void*) {
    ESP_LOGI(TAG, "TaskEventBus started");

    // All event types the bus knows about (matches EventType enum values 1..10)
    static constexpr EventType ALL_TYPES[] = {
        EventType::DEVICE_JOIN,
        EventType::DEVICE_LEAVE,
        EventType::ZCL_ATTR,
        EventType::ZCL_CMD,
        EventType::RULE_TRIGGER,
        EventType::CTRL_BOOT,
        EventType::ZCL_RAW,
        EventType::MQTT_MSG,
        EventType::RULE_EVENT,
        EventType::RULE_TIMER_FIRE,
    };

    while (true) {
        uint8_t processed = 0;
        for (EventType t : ALL_TYPES) {
            processed += event_bus_drain(t, 0);
        }
        // If no events, sleep briefly to yield CPU rather than spin
        if (processed == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void task_log_drain(void*) {
    ESP_LOGI(TAG, "TaskLog started");
    // Install the vprintf hook now that the queue exists and HAP is up
    esp_log_set_vprintf(log_vprintf_hook);

    LogEntry e{};
    while (true) {
        if (xQueueReceive(s_log_queue, &e, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        uint8_t buf[HAP_LOG_MSG_MAX + 16];
        uint16_t len = 0;
        if (!hap_json_encode_log_line(buf, sizeof(buf), &len, e.line)) {
            continue;
        }
        HapFrame f{};
        f.type        = HapMsgType::LOG_LINE;
        f.seq         = hap_session_next_seq();
        f.flags       = 0;  // fire-and-forget: no ACK for log lines
        f.payload     = buf;
        f.payload_len = len;
        hap_session_send(f);
    }
}

static void task_wdt(void*) {
    esp_task_wdt_add(nullptr);
    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ── Stack high-water-mark monitor (P2) ───────────────────────────────────
// Logs free stack bytes for each named task every 60 s. Use output to
// right-size stack allocations — free < 512 bytes is dangerously low.
static void task_stack_mon(void*) {
    vTaskDelay(pdMS_TO_TICKS(15000));  // let all tasks start
    while (true) {
        ESP_LOGI(TAG, "=== Stack HWM (P4) ===");
        for (const auto* e = zhac::stack::kTable; e->name != nullptr; ++e) {
            TaskHandle_t h = xTaskGetHandle(e->name);
            if (!h) continue;
            uint32_t free_bytes = uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t);
            if (free_bytes < 512) {
                ESP_LOGW(TAG, "  %-16s  LOW STACK: free=%" PRIu32 " bytes", e->name, free_bytes);
            }
            uint32_t used = (free_bytes > e->size) ? 0 : (e->size - free_bytes);
            ESP_LOGI(TAG, "  %-16s  total=%4" PRIu32 "  free=%4" PRIu32 "  used=%4" PRIu32,
                     e->name, e->size, free_bytes, used);
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

// ── Zigbee factory reset ─────────────────────────────────────────────────
// Erases all device data, rules, and scripts from NVS, then reboots.
// Called from button long-press or HAP ZIGBEE_FACTORY_RESET message.
void zigbee_factory_reset() {
    ESP_LOGW(TAG, "=== ZIGBEE FACTORY RESET ===");

    // Erase device store
    nvs_handle_t h;
    if (nvs_open("zap_v0", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
        ESP_LOGI(TAG, "Device store erased");
    }
    // Erase rule store
    if (nvs_open("zap_rules", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
        ESP_LOGI(TAG, "Rule store erased");
    }
    // Format the Lua script SPIFFS partition (`scripts`).
    if (esp_spiffs_format("scripts") == ESP_OK) {
        ESP_LOGI(TAG, "Lua script partition formatted");
    } else {
        ESP_LOGW(TAG, "esp_spiffs_format(scripts) failed — partition may not be mounted");
    }
    // Erase operator-configured Zigbee identity (channel + network
    // key). Absent next boot → do_commissioning regenerates a new
    // random key and defaults to channel 11.
    if (nvs_open("zigbee_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
        ESP_LOGI(TAG, "Zigbee identity erased (channel + network key)");
    }

    // Wipe the ZNP stick's own ZNP_HAS_CONFIGURED marker so the next
    // boot runs full BDB commissioning with the fresh channel + net
    // key. Without this, the stick keeps its internal NVS (PAN ID,
    // network key, device table) regardless of ESP-side NVS wipes —
    // and the MiBoxer-style ZB3 joins that depend on the new endpoint
    // table + TCSig fix would silently continue to fail.
    zigbee_force_recommission();

    ESP_LOGW(TAG, "Factory reset complete — rebooting in 1 s");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// ── P4 button monitor (reset + permit-join) ──────────────────────────────
static constexpr gpio_num_t RESET_BTN = (gpio_num_t)CONFIG_ZHAC_P4_RESET_BTN_GPIO;
static constexpr gpio_num_t JOIN_BTN  = (gpio_num_t)CONFIG_ZHAC_P4_JOIN_BTN_GPIO;
static constexpr uint32_t   LONG_PRESS_MS = 5000;

static void task_buttons(void*) {
    ESP_LOGI(TAG, "TaskButtons started — RESET=GPIO%d JOIN=GPIO%d", RESET_BTN, JOIN_BTN);

    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << RESET_BTN) | (1ULL << JOIN_BTN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
#endif
    };
    gpio_config(&gc);

    // If a button reads LOW at boot (never released), treat pin as unwired/floating
    // and disable its functionality to prevent accidental factory reset.
    vTaskDelay(pdMS_TO_TICKS(50));  // let pullups settle
    bool reset_wired = (gpio_get_level(RESET_BTN) != 0);
    bool join_wired  = (gpio_get_level(JOIN_BTN)  != 0);
    if (!reset_wired) ESP_LOGW(TAG, "RESET button GPIO%d reads LOW at boot — disabled (unwired)", RESET_BTN);
    if (!join_wired)  ESP_LOGW(TAG, "JOIN button GPIO%d reads LOW at boot — disabled (unwired)",  JOIN_BTN);

    uint32_t reset_press_ms = 0;
    bool     join_was_pressed = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));

        // ── Reset button: long-press 5 s → factory reset ────────────
        if (reset_wired && gpio_get_level(RESET_BTN) == 0) {
            reset_press_ms += 100;
            if (reset_press_ms >= LONG_PRESS_MS) {
                ESP_LOGW(TAG, "Reset button held %lu ms — factory reset",
                         (unsigned long)reset_press_ms);
                zigbee_factory_reset();  // does not return
            }
        } else {
            reset_press_ms = 0;
        }

        // ── Join button: short-press → permit join 254 s ────────────
        bool join_pressed = join_wired && (gpio_get_level(JOIN_BTN) == 0);
        if (join_was_pressed && !join_pressed) {
            // Button released — trigger permit join
            ESP_LOGI(TAG, "Join button pressed — opening network 254 s");
            zigbee_permit_join(254);
        }
        join_was_pressed = join_pressed;
    }
}

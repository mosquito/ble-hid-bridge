#include "config.h"
#include "task_base.h"
#include "storage.h"
#include "button_event.h"
#include "tasks/led.h"
#include "tasks/button.h"
#include "tasks/wifi.h"
#include "tasks/web_server.h"
#include "tasks/hid_bridge.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"

static const char* TAG = "main";

// Global storage instance
static Storage g_storage("app");

// Button handler context for WiFi toggle and LED indication
struct ButtonHandlerContext {
    Button* button;
    Wifi* wifi;
    WebServer* web_server;
    Led* led;
    HidBridge* hid;
};

// Button handler task - controls WiFi/WebServer via double-click and LED indication
static void buttonHandlerTask(void* params)
{
    auto* ctx = static_cast<ButtonHandlerContext*>(params);
    LedCmd current_led = { LedPattern::FADE, LED_COLOR_IDLE };
    ctx->led->set(current_led);
    bool reset_warning = false;

    LOG("btn_handler", "Task started on core %d", xPortGetCoreID());

    while (true) {
        // Factory reset: 15s hold = blink red warning, 30s = erase & reboot
        if (ctx->button->handleState(ButtonEvent::EXTRA_LONG_PRESS)) {
            reset_warning = true;
            ctx->led->set({ LedPattern::BLINK, LED_COLOR_DANGER });
            LOG("btn_handler", "Factory reset warning - release to cancel");
        }

        if (reset_warning) {
            if (ctx->button->handleState(ButtonEvent::FACTORY_RESET)) {
                ctx->led->set({ LedPattern::SOLID, LED_COLOR_DANGER });
                LOG("btn_handler", "FACTORY RESET");
                vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_DELAY_MS));
                nvs_flash_erase();
                esp_restart();
            }
            if (!ctx->button->isPressed()) {
                reset_warning = false;
                ctx->led->set(current_led);
                LOG("btn_handler", "Factory reset cancelled");
            }
            vTaskDelay(pdMS_TO_TICKS(BUTTON_HANDLER_POLL_MS));
            continue;
        }

        // Handle triple-click for headless BLE pairing
        if (ctx->button->handleState(ButtonEvent::TRIPLE_CLICK)) {
            BleCmdMsg cmd = {};
            cmd.cmd = BleCmd::SCAN_PAIR;
            xQueueSend(ctx->hid->getCmdQueue(), &cmd, 0);
            LOG("btn_handler", "Triple-click: scan & pair");
        }

        // Handle double-click for WiFi toggle
        if (ctx->button->handleState(ButtonEvent::DOUBLE_CLICK)) {
            if (ctx->wifi->isAPActive()) {
                ctx->web_server->stop();
                ctx->wifi->stopAP();
                LOG("btn_handler", "WiFi disabled");
            } else {
                ctx->wifi->startAP();
                vTaskDelay(pdMS_TO_TICKS(WIFI_TOGGLE_DELAY_MS));
                ctx->web_server->start();
                LOG("btn_handler", "WiFi enabled");
            }
        }

        // Update LED based on system state
        // Priority: Pairing > WiFi ON > BLE connected > Idle
        LedCmd target;
        if (ctx->hid->getPairState() != PairState::IDLE) {
            target = { LedPattern::FADE, LED_COLOR_CONNECTING };
        } else if (ctx->wifi->isAPActive()) {
            target = { LedPattern::BLINK, LED_COLOR_WIFI };
        } else if (ctx->hid->connectionCount() > 0) {
            target = { LedPattern::SOLID, LED_COLOR_CONNECTED };
        } else {
            target = { LedPattern::FADE, LED_COLOR_IDLE };
        }

        if (target != current_led) {
            current_led = target;
            ctx->led->set(target);
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_HANDLER_POLL_MS));
    }
}

extern "C" void app_main()
{
    LOG(TAG, "ESP32-S3 BLE-USB HID Bridge started");

    // Initialize storage
    g_storage.init();

    // Instantiate tasks on stack (valid forever - app_main never returns)
    static Led led(USER_LED_PIN);
    static ButtonConfig btn_cfg;
    static Button button(BUTTON_PIN, btn_cfg);
    static Wifi wifi(&g_storage);
    static WebServer web_server(&g_storage);

    // HID Bridge - combines BLE host and USB HID in one realtime task
    static HidBridge hid_bridge(&g_storage, web_server.getEventQueue());

    // Connect WebServer to HID Bridge
    web_server.setBleCmdQueue(hid_bridge.getCmdQueue());
    web_server.setHidBridge(&hid_bridge);

    // Start LED task
    xTaskCreatePinnedToCore(
        TaskBase::taskEntry,
        "led",
        LED_TASK_STACK_SIZE,
        &led,
        RTOS_PRIORITY_LOW,
        nullptr,
        RTOS_CORE_0
    );

    // Start Button task
    xTaskCreatePinnedToCore(
        TaskBase::taskEntry,
        "button",
        BUTTON_TASK_STACK_SIZE,
        &button,
        RTOS_PRIORITY_LOW,
        nullptr,
        RTOS_CORE_0
    );

    // Start WiFi task
    xTaskCreatePinnedToCore(
        TaskBase::taskEntry,
        "wifi",
        WIFI_TASK_STACK_SIZE,
        &wifi,
        RTOS_PRIORITY_LOW,
        nullptr,
        RTOS_CORE_0
    );

    // Start WebServer task (Core 0 - non-critical)
    xTaskCreatePinnedToCore(
        TaskBase::taskEntry,
        "web",
        WEB_TASK_STACK_SIZE,
        &web_server,
        RTOS_PRIORITY_MEDIUM,
        nullptr,
        RTOS_CORE_0
    );

    // Start HID Bridge task (Core 1 - dedicated, nothing else on this core)
    xTaskCreatePinnedToCore(
        TaskBase::taskEntry,
        "hid_bridge",
        BLE_TASK_STACK_SIZE,
        &hid_bridge,
        RTOS_PRIORITY_MEDIUM,
        nullptr,
        RTOS_CORE_1
    );

    // Button handler context (static so it lives forever)
    static ButtonHandlerContext btn_ctx = {
        .button = &button,
        .wifi = &wifi,
        .web_server = &web_server,
        .led = &led,
        .hid = &hid_bridge
    };

    // Start button handler task (controls WiFi toggle via double-click)
    xTaskCreatePinnedToCore(
        buttonHandlerTask,
        "btn_handler",
        DEFAULT_TASK_STACK_SIZE,
        &btn_ctx,
        RTOS_PRIORITY_LOW,
        nullptr,
        RTOS_CORE_0
    );

    LOG(TAG, "All tasks started");

    // Main loop - nothing to do, tasks run independently
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

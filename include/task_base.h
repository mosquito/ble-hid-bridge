#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Base class for FreeRTOS tasks
 *
 * Usage:
 *   class MyTask : public TaskBase {
 *   public:
 *       void init() override { ... }
 *       void run() override { ... }
 *   };
 *
 *   MyTask my_task;
 *   xTaskCreate(TaskBase::taskEntry, "name", stack, &my_task, prio, nullptr);
 */
class TaskBase {
public:
    virtual ~TaskBase() = default;

    /**
     * Initialize the task. Called once before the loop starts.
     */
    virtual void init() = 0;

    /**
     * Run one iteration. Called repeatedly in the task loop.
     * Use vTaskDelay() inside if needed.
     */
    virtual void run() = 0;

    /**
     * The FreeRTOS task loop. Calls init() once, then run() forever.
     */
    [[noreturn]] void task()
    {
        init();
        while (true) {
            run();
        }
    }

    /**
     * Static entry point for xTaskCreate.
     * Pass 'this' pointer as pvParameters.
     */
    static void taskEntry(void* pvParameters)
    {
        static_cast<TaskBase*>(pvParameters)->task();
    }
};

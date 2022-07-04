// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "language.h"

namespace skyline {
    /**
     * @brief The Settings class provides a simple interface to access user-defined settings, update values and subscribe callbacks to observe changes.
     */
    class Settings {
        template<typename T>
        class Setting {
            using Callback = std::function<void(const T &)>;
            std::vector<Callback> callbacks; //!< Callbacks to be called when this setting changes
            T value;

            /**
             * @brief Calls all registered callbacks
             */
            void OnSettingChanged() {
                std::for_each(callbacks.begin(), callbacks.end(), [&](const Callback &callback) {
                    callback(value);
                });
            }

          public:
            /**
             * @brief Getter of the underlying setting value
             */
            const T &operator*() const {
                return value;
            }

            /**
             * @brief Setter of the underlying setting value
             */
            void operator=(T newValue) {
                if (value != newValue) {
                    value = std::move(newValue);
                    OnSettingChanged();
                }
            }

            /**
             * @brief Register a callback to run when this setting changes
             */
            void AddCallback(Callback callback) {
                callbacks.push_back(std::move(callback));
            }
        };

      public:
        // System
        Setting<bool> isDocked; //!< If the emulated Switch should be handheld or docked
        Setting<std::string> usernameValue; //!< The name set by the user to be supplied to the guest
        Setting<language::SystemLanguage> systemLanguage; //!< The system language set by the user

        // Display
        Setting<bool> forceTripleBuffering; //!< If the presentation engine should always triple buffer even if the swapchain supports double buffering
        Setting<bool> disableFrameThrottling; //!< Allow the guest to submit frames without any blocking calls

        Settings() {}

        virtual ~Settings() = default;

        /**
         * @brief Updates settings with the given values
         * @note This method is platform-specific and must be overridden
         */
        virtual void Update() = 0;
    };
}

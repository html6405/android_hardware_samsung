/*
 * Copyright (C) 2013 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.cyanogenmod.hardware;

import org.cyanogenmod.hardware.util.FileUtils;

import android.os.SystemProperties;
import android.text.TextUtils;

import java.io.File;
import android.util.Log;

/**
 * Adaptive backlight support (this refers to technologies like NVIDIA SmartDimmer,
 * QCOM CABL or Samsung CABC).
 */
public class AdaptiveBacklight {

    private static String FILE_CABC = SystemProperties.get("ro.cm.hardware.cabc", "/sys/class/lcd/panel/power_reduce");
    private static final String TAG = "cmhw.AdaptiveBacklight";
    
    /**
     * Whether device supports an adaptive backlight technology.
     *
     * @return boolean Supported devices must return always true
     */
    public static boolean isSupported() {
        File f = new File(FILE_CABC);
        if(f.exists())
        {
            Log.i(TAG,"Detected AdaptiveBacklight!");
            return true;
        }
        else
        {
            Log.i(TAG,"AdaptiveBacklight not detected!");
            return false;
        }
    }

    /**
     * This method return the current activation status of the adaptive backlight technology.
     *
     * @return boolean Must be false when adaptive backlight is not supported or not activated, or
     * the operation failed while reading the status; true in any other case.
     */
    public static boolean isEnabled() {
        if (TextUtils.equals(FileUtils.readOneLine(FILE_CABC), "1")) {
            Log.i(TAG,"AdaptiveBacklight is enabled");
            return true;
        } else {
            Log.i(TAG,"AdaptiveBacklight is not enabled");
            return false;
        }
    }

    /**
     * This method allows to setup adaptive backlight technology status.
     *
     * @param status The new adaptive backlight status
     * @return boolean Must be false if adaptive backlight is not supported or the operation
     * failed; true in any other case.
     */
    public static boolean setEnabled(boolean status) {
        if (status == true) {
            Log.i(TAG,"Enabling AdaptiveBacklight");
            return FileUtils.writeLine(FILE_CABC, "1");
        } else {
            Log.i(TAG,"Disabling AdaptiveBacklight");
            return FileUtils.writeLine(FILE_CABC, "0");
        }
    }
}

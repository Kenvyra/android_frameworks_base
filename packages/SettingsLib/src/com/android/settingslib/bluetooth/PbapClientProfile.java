/*
 * Copyright (C) 2016 The Android Open Source Project
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

package com.android.settingslib.bluetooth;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothPbapClient;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothUuid;
import android.content.Context;
import android.os.ParcelUuid;
import android.util.Log;

import com.android.settingslib.R;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

public final class PbapClientProfile implements LocalBluetoothProfile {
    private static final String TAG = "PbapClientProfile";

    private BluetoothPbapClient mService;
    private boolean mIsProfileReady;

    private final CachedBluetoothDeviceManager mDeviceManager;

    static final ParcelUuid[] SRC_UUIDS = {
        BluetoothUuid.PBAP_PSE,
    };

    static final String NAME = "PbapClient";
    private final LocalBluetoothProfileManager mProfileManager;

    // Order of this profile in device profiles list
    private static final int ORDINAL = 6;

    // These callbacks run on the main thread.
    private final class PbapClientServiceListener
            implements BluetoothProfile.ServiceListener {

        public void onServiceConnected(int profile, BluetoothProfile proxy) {
            Log.d(TAG, "Bluetooth service connected, profile:" + profile);
            mService = (BluetoothPbapClient) proxy;
            // We just bound to the service, so refresh the UI for any connected PBAP devices.
            List<BluetoothDevice> deviceList = mService.getConnectedDevices();
            while (!deviceList.isEmpty()) {
                BluetoothDevice nextDevice = deviceList.remove(0);
                CachedBluetoothDevice device = mDeviceManager.findDevice(nextDevice);
                // we may add a new device here, but generally this should not happen
                if (device == null) {
                    Log.w(TAG, "PbapClientProfile found new device: " + nextDevice);
                    device = mDeviceManager.addDevice(nextDevice);
                }
                device.onProfileStateChanged(PbapClientProfile.this, BluetoothProfile.STATE_CONNECTED);
                device.refresh();
            }
            mIsProfileReady = true;
        }

        public void onServiceDisconnected(int profile) {
            Log.d(TAG, "Bluetooth service disconnected, profile:" + profile);
            mIsProfileReady = false;
        }
    }

    private void refreshProfiles() {
        Collection<CachedBluetoothDevice> cachedDevices = mDeviceManager.getCachedDevicesCopy();
        for (CachedBluetoothDevice device : cachedDevices) {
            device.onUuidChanged();
        }
    }

    public boolean pbapClientExists() {
        return (mService != null);
    }

    public boolean isProfileReady() {
        return mIsProfileReady;
    }

    @Override
    public int getProfileId() {
        return BluetoothProfile.PBAP_CLIENT;
    }

    PbapClientProfile(Context context, CachedBluetoothDeviceManager deviceManager,
            LocalBluetoothProfileManager profileManager) {
        mDeviceManager = deviceManager;
        mProfileManager = profileManager;
        BluetoothAdapter.getDefaultAdapter().getProfileProxy(context,
                new PbapClientServiceListener(), BluetoothProfile.PBAP_CLIENT);
    }

    public boolean accessProfileEnabled() {
        return true;
    }

    public boolean isAutoConnectable() {
        return true;
    }

    public List<BluetoothDevice> getConnectedDevices() {
        if (mService == null) {
            return new ArrayList<BluetoothDevice>(0);
        }
        return mService.getDevicesMatchingConnectionStates(
              new int[] {BluetoothProfile.STATE_CONNECTED,
                         BluetoothProfile.STATE_CONNECTING,
                         BluetoothProfile.STATE_DISCONNECTING});
    }

    public boolean connect(BluetoothDevice device) {
        Log.d(TAG,"PBAPClientProfile got connect request");
        if (mService == null) {
            return false;
        }
        Log.d(TAG,"PBAPClientProfile attempting to connect to " + device.getAddress());
        return mService.connect(device);
    }

    public boolean disconnect(BluetoothDevice device) {
        Log.d(TAG,"PBAPClientProfile got disconnect request");
        if (mService == null) {
            return false;
        }
        return mService.disconnect(device);
    }

    public int getConnectionStatus(BluetoothDevice device) {
        if (mService == null) {
            return BluetoothProfile.STATE_DISCONNECTED;
        }
        return mService.getConnectionState(device);
    }

    public boolean isPreferred(BluetoothDevice device) {
        if (mService == null) {
            return false;
        }
        return mService.getPriority(device) > BluetoothProfile.PRIORITY_OFF;
    }

    public int getPreferred(BluetoothDevice device) {
        if (mService == null) {
            return BluetoothProfile.PRIORITY_OFF;
        }
        return mService.getPriority(device);
    }

    public void setPreferred(BluetoothDevice device, boolean preferred) {
        if (mService == null) {
            return;
        }
        if (preferred) {
            if (mService.getPriority(device) < BluetoothProfile.PRIORITY_ON) {
                mService.setPriority(device, BluetoothProfile.PRIORITY_ON);
            }
        } else {
            mService.setPriority(device, BluetoothProfile.PRIORITY_OFF);
        }
    }

    public String toString() {
        return NAME;
    }

    public int getOrdinal() {
        return ORDINAL;
    }

    public int getNameResource(BluetoothDevice device) {
        // we need to have same string in UI as the server side.
        return R.string.bluetooth_profile_pbap;
    }

    public int getSummaryResourceForDevice(BluetoothDevice device) {
        return R.string.bluetooth_profile_pbap_summary;
    }

    public int getDrawableResource(BluetoothClass btClass) {
        return R.drawable.ic_bt_cellphone;
    }

    protected void finalize() {
        Log.d(TAG, "finalize()");
        if (mService != null) {
            try {
                BluetoothAdapter.getDefaultAdapter().closeProfileProxy(
                    BluetoothProfile.PBAP_CLIENT,mService);
                mService = null;
            } catch (Throwable t) {
                Log.w(TAG, "Error cleaning up PBAP Client proxy", t);
            }
        }
    }
}

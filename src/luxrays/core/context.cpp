/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <cstdlib>
#include <cassert>
#include <iosfwd>
#include <sstream>
#include <stdexcept>

#include "luxrays/core/context.h"
#include "luxrays/devices/nativeintersectiondevice.h"
#if !defined(LUXRAYS_DISABLE_OPENCL)
#include "luxrays/devices/ocldevice.h"
#include "luxrays/devices/oclintersectiondevice.h"
#endif

using namespace std;
using namespace luxrays;

Context::Context(LuxRaysDebugHandler handler, const Properties &config) : cfg(config) {
	debugHandler = handler;
	currentDataSet = NULL;
	started = false;
	verbose = cfg.Get(Property("context.verbose")(true)).Get<bool>();

	// Get the list of devices available on the platform
	NativeIntersectionDeviceDescription::AddDeviceDescs(deviceDescriptions);

#if !defined(LUXRAYS_DISABLE_OPENCL)
	// Platform info
	VECTOR_CLASS<cl::Platform> platforms;
	try {
		cl::Platform::get(&platforms);
	} catch (cl::Error &err) {
		// The cl_khr_icd extension throws exceptions if zero platforms are available.
		// We ignore that error (OpenCL is optional), but throw anything else.
#if defined(cl_khr_idc)
        if (err.err() != CL_PLATFORM_NOT_FOUND_KHR)
            throw;
#endif
	}
	for (size_t i = 0; i < platforms.size(); ++i)
		LR_LOG(this, "OpenCL Platform " << i << ": " << platforms[i].getInfo<CL_PLATFORM_VENDOR>().c_str());

	const int openclPlatformIndex = cfg.Get(Property("context.opencl.platform.index")(-1)).Get<int>();
	if (openclPlatformIndex < 0) {
		if (platforms.size() > 0) {
			// Just use all the platforms available
			for (size_t i = 0; i < platforms.size(); ++i)
				OpenCLDeviceDescription::AddDeviceDescs(
					platforms[i], DEVICE_TYPE_OPENCL_ALL,
					deviceDescriptions);
		} else
			LR_LOG(this, "No OpenCL platform available");
	} else {
		if ((platforms.size() == 0) || (openclPlatformIndex >= (int)platforms.size()))
			throw runtime_error("Unable to find an appropriate OpenCL platform");
		else {
			OpenCLDeviceDescription::AddDeviceDescs(
				platforms[openclPlatformIndex],
				DEVICE_TYPE_OPENCL_ALL, deviceDescriptions);
		}
	}
#endif

	// Print device info
	for (size_t i = 0; i < deviceDescriptions.size(); ++i) {
		DeviceDescription *desc = deviceDescriptions[i];
		LR_LOG(this, "Device " << i << " name: " <<
			desc->GetName());

		LR_LOG(this, "Device " << i << " type: " <<
			DeviceDescription::GetDeviceType(desc->GetType()));

		LR_LOG(this, "Device " << i << " compute units: " <<
			desc->GetComputeUnits());

		LR_LOG(this, "Device " << i << " preferred float vector width: " <<
			desc->GetNativeVectorWidthFloat());

		LR_LOG(this, "Device " << i << " max allocable memory: " <<
			desc->GetMaxMemory() / (1024 * 1024) << "MBytes");

		LR_LOG(this, "Device " << i << " max allocable memory block size: " <<
			desc->GetMaxMemoryAllocSize() / (1024 * 1024) << "MBytes");
	}
}

Context::~Context() {
	if (started)
		Stop();

	for (size_t i = 0; i < devices.size(); ++i)
		delete devices[i];
	for (size_t i = 0; i < deviceDescriptions.size(); ++i)
		delete deviceDescriptions[i];
}

void Context::SetDataSet(DataSet *dataSet) {
	assert (!started);

	currentDataSet = dataSet;

	for (size_t i = 0; i < idevices.size(); ++i)
		idevices[i]->SetDataSet(currentDataSet);
}

void Context::UpdateDataSet() {
	assert (started);

	// Update the data set
	currentDataSet->UpdateAccelerators();

#if !defined(LUXRAYS_DISABLE_OPENCL)
	// Update all OpenCL devices
	for (u_int i = 0; i < idevices.size(); ++i) {
		OpenCLIntersectionDevice *oclDevice = dynamic_cast<OpenCLIntersectionDevice *>(idevices[i]);
		if (oclDevice)
			oclDevice->Update();
	}
#endif
}

void Context::Start() {
	assert (!started);

	for (size_t i = 0; i < devices.size(); ++i)
		devices[i]->Start();

	started = true;
}

void Context::Interrupt() {
	assert (started);

	for (size_t i = 0; i < devices.size(); ++i)
		devices[i]->Interrupt();
}

void Context::Stop() {
	assert (started);

	Interrupt();

	for (size_t i = 0; i < devices.size(); ++i)
		devices[i]->Stop();

	started = false;
}

const vector<DeviceDescription *> &Context::GetAvailableDeviceDescriptions() const {
	return deviceDescriptions;
}

const vector<IntersectionDevice *> &Context::GetIntersectionDevices() const {
	return idevices;
}

const vector<HardwareDevice *> &Context::GetHardwareDevices() const {
	return hdevices;
}

const vector<Device *> &Context::GetDevices() const {
	return devices;
}

vector<IntersectionDevice *> Context::CreateIntersectionDevices(
	vector<DeviceDescription *> &deviceDesc, const size_t indexOffset) {
	assert (!started);

	LR_LOG(this, "Creating " << deviceDesc.size() << " intersection device(s)");

	vector<IntersectionDevice *> newDevices;
	for (size_t i = 0; i < deviceDesc.size(); ++i) {
		LR_LOG(this, "Allocating intersection device " << i << ": " << deviceDesc[i]->GetName() <<
				" (Type = " << DeviceDescription::GetDeviceType(deviceDesc[i]->GetType()) << ")");

		const DeviceType deviceType = deviceDesc[i]->GetType();
		IntersectionDevice *device;
		if (deviceType == DEVICE_TYPE_NATIVE) {
			// Nathive thread devices
			device = new NativeIntersectionDevice(this, indexOffset + i);
		}
#if !defined(LUXRAYS_DISABLE_OPENCL)
		else if (deviceType & DEVICE_TYPE_OPENCL_ALL) {
			// OpenCL devices
			OpenCLDeviceDescription *oclDeviceDesc = (OpenCLDeviceDescription *)deviceDesc[i];

			device = new OpenCLIntersectionDevice(this, oclDeviceDesc, indexOffset + i);
		}
#endif
		else
			throw runtime_error("Unknown device type in Context::CreateIntersectionDevices(): " + ToString(deviceType));

		newDevices.push_back(device);
	}

	return newDevices;
}

vector<IntersectionDevice *> Context::AddIntersectionDevices(vector<DeviceDescription *> &deviceDesc) {
	assert (!started);

	vector<IntersectionDevice *> newDevices = CreateIntersectionDevices(deviceDesc, idevices.size());
	for (size_t i = 0; i < newDevices.size(); ++i) {
		idevices.push_back(newDevices[i]);
		devices.push_back(newDevices[i]);
	}

	return newDevices;
}

vector<HardwareDevice *> Context::CreateHardwareDevices(
	vector<DeviceDescription *> &deviceDesc, const size_t indexOffset) {
	assert (!started);

	LR_LOG(this, "Creating " << deviceDesc.size() << " hardware device(s)");

	vector<HardwareDevice *> newDevices;
	for (size_t i = 0; i < deviceDesc.size(); ++i) {
		LR_LOG(this, "Allocating hardware device " << i << ": " << deviceDesc[i]->GetName() <<
				" (Type = " << DeviceDescription::GetDeviceType(deviceDesc[i]->GetType()) << ")");

		const DeviceType deviceType = deviceDesc[i]->GetType();
		HardwareDevice *device;
		if (deviceType == DEVICE_TYPE_NATIVE) {
			throw runtime_error("Native devices are not supported as hardware devices in Context::CreateHardwareDevices()");
		}
#if !defined(LUXRAYS_DISABLE_OPENCL)
		else if (deviceType & DEVICE_TYPE_OPENCL_ALL) {
			// OpenCL devices
			OpenCLDeviceDescription *oclDeviceDesc = (OpenCLDeviceDescription *)deviceDesc[i];

			device = new OpenCLDevice(this, oclDeviceDesc, indexOffset + i);
		}
#endif
		else
			throw runtime_error("Unknown device type in Context::CreateHardwareDevices(): " + ToString(deviceType));

		newDevices.push_back(device);
	}

	return newDevices;
}

vector<HardwareDevice *> Context::AddHardwareDevices(vector<DeviceDescription *> &deviceDesc) {
	assert (!started);

	vector<HardwareDevice *> newDevices = CreateHardwareDevices(deviceDesc, hdevices.size());
	for (size_t i = 0; i < newDevices.size(); ++i) {
		hdevices.push_back(newDevices[i]);
		devices.push_back(newDevices[i]);
	}

	return newDevices;
}

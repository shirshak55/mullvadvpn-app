#include "stdafx.h"
#include <filesystem>
#include <iostream>
#include <optional>
#include <libcommon/error.h>
#include <libcommon/memory.h>
#include <setupapi.h>
#include <devguid.h>
#include <newdev.h>


namespace
{

const wchar_t TAP_HARDWARE_ID[] = L"tap0901";

enum ReturnCodes
{
	MULLVAD_GENERAL_ERROR,
	MULLVAD_SUCCESS
};

std::optional<std::wstring> GetDeviceRegistryStringProperty(
	HDEVINFO devInfo,
	SP_DEVINFO_DATA *devInfoData,
	DWORD property
)
{
	//
	// Obtain required buffer size
	//

	DWORD requiredSize = 0;

	const auto sizeStatus = SetupDiGetDeviceRegistryPropertyW(
		devInfo,
		devInfoData,
		property,
		nullptr,
		nullptr,
		0,
		&requiredSize
	);

	const DWORD lastError = GetLastError();
	if (FALSE == sizeStatus && ERROR_INSUFFICIENT_BUFFER != lastError)
	{
		// ERROR_INVALID_DATA may mean that the property does not exist
		// TODO: Check if there may be other causes.
		THROW_UNLESS(ERROR_INVALID_DATA, lastError, "SetupDiGetDeviceRegistryPropertyW");
		return std::nullopt;
	}

	//
	// Read property
	//

	std::vector<wchar_t> buffer;
	buffer.resize(1 + requiredSize / sizeof(wchar_t));

	const auto status = SetupDiGetDeviceRegistryPropertyW(
		devInfo,
		devInfoData,
		property,
		nullptr,
		reinterpret_cast<PBYTE>(&buffer[0]),
		static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
		nullptr
	);

	THROW_GLE_IF(FALSE, status, "Failed to read device property");

	return { buffer.data() };
}

std::wstring GetDeviceStringProperty(
	HDEVINFO devInfo,
	SP_DEVINFO_DATA *devInfoData,
	const DEVPROPKEY *property
)
{
	//
	// Obtain required buffer size
	//

	DWORD requiredSize = 0;
	DEVPROPTYPE type;

	const auto sizeStatus = SetupDiGetDevicePropertyW(
		devInfo,
		devInfoData,
		property,
		&type,
		nullptr,
		0,
		&requiredSize,
		0
	);

	if (FALSE == sizeStatus)
	{
		THROW_UNLESS(ERROR_INSUFFICIENT_BUFFER, GetLastError(), "SetupDiGetDevicePropertyW");
	}

	std::vector<wchar_t> buffer;
	buffer.resize(1 + requiredSize / sizeof(wchar_t));

	//
	// Read property
	//

	const auto status = SetupDiGetDevicePropertyW(
		devInfo,
		devInfoData,
		property,
		&type,
		reinterpret_cast<PBYTE>(&buffer[0]),
		static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
		nullptr,
		0
	);

	THROW_GLE_IF(FALSE, status, "Failed to read device property");

	return buffer.data();
}

void addTapDevice(const wchar_t *deviceId)
{
	GUID classGuid = GUID_DEVCLASS_NET;

	const auto deviceInfoSet = SetupDiCreateDeviceInfoList(&classGuid, 0);
	THROW_GLE_IF(INVALID_HANDLE_VALUE, deviceInfoSet, "SetupDiCreateDeviceInfoList()");

	common::memory::ScopeDestructor scopeDestructor;
	scopeDestructor += [&deviceInfoSet]()
	{
		SetupDiDestroyDeviceInfoList(deviceInfoSet);
	};

	SP_DEVINFO_DATA devInfoData;
	devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

	auto status = SetupDiCreateDeviceInfoW(
		deviceInfoSet,
		deviceId != nullptr ? deviceId : L"NET",
		&classGuid,
		nullptr,
		0,
		deviceId != nullptr ? 0 : DICD_GENERATE_ID,
		&devInfoData
	);
	THROW_GLE_IF(FALSE, status, "SetupDiCreateDeviceInfo()");

	status = SetupDiSetDeviceRegistryPropertyW(
		deviceInfoSet,
		&devInfoData,
		SPDRP_HARDWAREID,
		reinterpret_cast<const BYTE *>(TAP_HARDWARE_ID),
		sizeof(TAP_HARDWARE_ID) - sizeof(L'\0')
	);
	THROW_GLE_IF(FALSE, status, "SetupDiSetDeviceRegistryProperty()");

	//
	// Create a devnode in the PnP HW tree
	//
	status = SetupDiCallClassInstaller(
		DIF_REGISTERDEVICE,
		deviceInfoSet,
		&devInfoData
	);
	THROW_GLE_IF(FALSE, status, "SetupDiCallClassInstaller()");

	std::wcout << L"SetupDiCallClassInstaller() succeeded" << std::endl;
}

void recreateTapDevice(HDEVINFO devInfo, PSP_DEVINFO_DATA devInfoData)
{
	//
	// Obtain device instance ID
	//

	std::wstring deviceInstanceId;
	DWORD requiredSize = 0;

	SetupDiGetDeviceInstanceIdW(
		devInfo,
		devInfoData,
		nullptr,
		0,
		&requiredSize
	);

	deviceInstanceId.resize(requiredSize);

	auto status = SetupDiGetDeviceInstanceIdW(
		devInfo,
		devInfoData,
		&deviceInstanceId[0],
		static_cast<DWORD>(deviceInstanceId.size()),
		nullptr
	);
	THROW_GLE_IF(FALSE, status, "SetupDiGetDeviceInstanceIdW()");

	//
	// Delete existing device
	//

	SP_REMOVEDEVICE_PARAMS rmdParams;
	rmdParams.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
	rmdParams.ClassInstallHeader.InstallFunction = DIF_REMOVE;
	rmdParams.Scope = DI_REMOVEDEVICE_GLOBAL;
	rmdParams.HwProfile = 0;

	status = SetupDiSetClassInstallParamsW(devInfo, devInfoData, &rmdParams.ClassInstallHeader, sizeof(rmdParams));
	THROW_GLE_IF(FALSE, status, "SetupDiSetClassInstallParamsW()");

	status = SetupDiCallClassInstaller(DIF_REMOVE, devInfo, devInfoData);
	THROW_GLE_IF(FALSE, status, "SetupDiCallClassInstaller()");

	//
	// Create new device with the same ID
	//

	addTapDevice(deviceInstanceId.c_str());
}

void recreateAllTaps()
{
	HDEVINFO devInfo = SetupDiGetClassDevsW(
		&GUID_DEVCLASS_NET,
		nullptr,
		nullptr,
		DIGCF_PRESENT
	);
	THROW_GLE_IF(INVALID_HANDLE_VALUE, devInfo, "SetupDiGetClassDevsW() failed");

	common::memory::ScopeDestructor scopeDestructor;
	scopeDestructor += [devInfo]()
	{
		SetupDiDestroyDeviceInfoList(devInfo);
	};

	for (int memberIndex = 0; ; memberIndex++)
	{
		SP_DEVINFO_DATA devInfoData = { 0 };
		devInfoData.cbSize = sizeof(devInfoData);

		if (FALSE == SetupDiEnumDeviceInfo(devInfo, memberIndex, &devInfoData))
		{
			if (ERROR_NO_MORE_ITEMS == GetLastError())
			{
				// Done
				break;
			}
			THROW_GLE("SetupDiEnumDeviceInfo() failed while enumerating network adapters");
		}

		//
		// Check whether this is a TAP adapter
		//

		const auto hardwareId = GetDeviceRegistryStringProperty(devInfo, &devInfoData, SPDRP_HARDWAREID);
		if (!hardwareId.has_value()
			|| wcscmp(hardwareId.value().c_str(), TAP_HARDWARE_ID) != 0)
		{
			continue;
		}

		//
		// Replace TAP with new device
		//

		try
		{
			recreateTapDevice(devInfo, &devInfoData);
		}
		catch (const std::exception & e)
		{
			//
			// Log exception and skip this adapter
			//

			std::string msg = "Skipping TAP adapter due to exception caught while iterating: ";
			msg.append(e.what());
			std::cout << msg << std::endl;
		}
	}
}

void updateTapDriver()
{
	std::wcout << L"Attempting to install new driver" << std::endl;

	// TODO: do not hardcode inf path
	auto tempDir = std::filesystem::temp_directory_path();
	auto infPath = tempDir / L"driver" / L"OemVista.inf";

	DWORD installFlags = 0;
	BOOL rebootRequired = FALSE;

ATTEMPT_UPDATE:

	auto result = UpdateDriverForPlugAndPlayDevicesW(
		GetActiveWindow(),
		TAP_HARDWARE_ID,
		infPath.c_str(),
		installFlags,
		&rebootRequired
	);

	if (TRUE != result)
	{
		const auto lastError = GetLastError();

		if (ERROR_NO_MORE_ITEMS == lastError
			&& (installFlags ^ INSTALLFLAG_FORCE))
		{
			std::wcout << L"Driver update failed. Attempting forced install." << std::endl;
			installFlags |= INSTALLFLAG_FORCE;

			goto ATTEMPT_UPDATE;
		}

		common::error::Throw("UpdateDriverForPlugAndPlayDevicesW()", lastError);
	}

	//
	// Driver successfully installed or updated
	//

	std::wcout << L"TAP driver update complete. Reboot required: "
		<< rebootRequired << std::endl;
}

} // anonymous namespace

int main(int argc, const char * argv[])
{
	// TODO: logsink? is the last string from stderr/stdout returned to nsis? may be sufficient w/ error code

	if (2 != argc)
	{
		std::wcerr << L"Invalid arguments." << std::endl;
		return MULLVAD_GENERAL_ERROR;
	}

	//
	// Run setup
	//

	try
	{
		if (0 == _stricmp(argv[1], "create-tap"))
		{
			// FIXME: check existing driver; recreate devices only if the driver outdated
			//        with an exception for 2019.10 somehow...
			recreateAllTaps();
			addTapDevice(nullptr);
			updateTapDriver();
		}
		else if (0 == _stricmp(argv[1], "update"))
		{
			recreateAllTaps();
			updateTapDriver();
		}
		else
		{
			std::wcerr << L"Invalid arguments." << std::endl;
			return MULLVAD_GENERAL_ERROR;
		}
	}
	catch (const std::exception &e)
	{
		std::wcerr << e.what() << std::endl;
		return MULLVAD_GENERAL_ERROR;
	}
	catch (...)
	{
		std::wcerr << L"Unhandled exception." << std::endl;
		return MULLVAD_GENERAL_ERROR;
	}
	return MULLVAD_SUCCESS;
}

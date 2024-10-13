// CloudTest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "stdafx.h"

#include "CloudTest.h"
#include <iostream>

#include "Utilities.h"
#include "CloudProviderRegistrar.h"
#include <filesystem>
#include <conio.h>
#include <system_error>
#include <cassert>

CF_CONNECTION_KEY s_transferCallbackConnectionKey;
HRESULT Init(std::wstring localRoot);
HRESULT CreatePlaceHolder(_In_ std::wstring localRoot, _In_ PCWSTR parentPath, _In_ std::wstring fileName, uint64_t size);
void DisconnectSyncRootTransferCallbacks();
void ConnectSyncRootTransferCallbacks(std::wstring localRoot);
void TestSimpleRead(const std::wstring& filePath);
void TestFileMapping(const std::wstring& filePath);

int main()
{
    assert(CoInitialize(nullptr) == S_OK);

    std::cout << "Cloud sample test!\n";

    wchar_t tmpDir[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, tmpDir) != 0);

    wchar_t tmpDirLock[MAX_PATH];
    assert(GetTempFileNameW(tmpDir, L"cf-", 0, tmpDirLock) != 0);

    std::wstring workingDir = std::wstring(tmpDirLock) + L"-dir";
    assert(CreateDirectoryW(workingDir.c_str(), nullptr));

    Init(workingDir);

    CF_PLATFORM_INFO platformInfo;
    CfGetPlatformInfo(&platformInfo);

    std::cout << "platform version is " <<
        platformInfo.BuildNumber << "." <<
        platformInfo.RevisionNumber << "." <<
        platformInfo.IntegrationNumber << std::endl;

    auto testFileName = L"test1.txt";
    auto testFile = workingDir + L"\\" + testFileName;
    auto hr = CreatePlaceHolder(workingDir, L"", testFileName, 128*1024*1024);
    assert(hr == S_OK);

    TestSimpleRead(testFile);
    TestFileMapping(testFile);

    DisconnectSyncRootTransferCallbacks();
    CoUninitialize();
    
    std::filesystem::remove_all(workingDir);
    std::filesystem::remove(tmpDirLock);

    system("pause");

    return 0;
}

//
// create a handle to file and read first 4096 bytes using ReadFile
//
void TestSimpleRead(const std::wstring& filePath)
{
    size_t numBytesToRead = 4096;
    std::cout << "TestSimpleRead: read first " << numBytesToRead << "using ReadFile api" << std::endl;

    auto fileHandle = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        // check that Antivirus is disabled, Microsoft Defender is known to cause FetchData invocation due to real-time protection option
        assert(false);
    }

    // the call will fail but we should see how much data was requested
    std::vector<char> buffer(numBytesToRead);
    DWORD bytesRead = 0;
    auto res = ReadFile(fileHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr);
    assert(res == FALSE);
    CloseHandle(fileHandle);
    std::cout << "TestSimpleRead done\n";
}

//
// create a handle to file and read first 4096 bytes using CreateFileMapping and MapViewOfFile
//
void TestFileMapping(const std::wstring& filePath)
{
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    HANDLE fileMapping = nullptr;
    LPVOID fileView = nullptr;
    size_t numBytesToRead = 4096;

    std::cout << "TestFileMapping: read first " << numBytesToRead << "using CreateFileMapping and MapViewOfFile api" << std::endl;

    fileHandle = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        // check that Antivirus is disabled, Microsoft Defender is known to cause FetchData invocation due to real-time protection option
        assert(false);
    }

    // CreateFileMapping causes full hydration no matter what
    fileMapping = CreateFileMapping(fileHandle, nullptr, PAGE_READONLY, 0, static_cast<DWORD>(numBytesToRead), nullptr);
    if (fileMapping == nullptr)
        goto cleanup;

    fileView = MapViewOfFile(fileMapping, FILE_MAP_READ, 0, 0, numBytesToRead);
    if (fileView == nullptr)
        goto cleanup;

    // read first numBytesToRead bytes
    for (int i = 0; i < numBytesToRead; i++)
        std::cout << static_cast<char*>(fileView)[i];

cleanup:
    if (fileView != nullptr)
        UnmapViewOfFile(fileView);
    if (fileMapping != nullptr)
        CloseHandle(fileMapping);
    CloseHandle(fileHandle);
    std::cout << "TestFileMapping done\n";
}

HRESULT Mount(std::wstring localRoot) {

    // Stage 1: Setup
    //--------------------------------------------------------------------------------------------
    // The client folder (syncroot) must be indexed in order for states to properly display
    Utilities::AddFolderToSearchIndexer(localRoot.c_str());


    concurrency::create_task([localRoot] {
        // Register the provider with the shell so that the Sync Root shows up in File Explorer
        CloudProviderRegistrar::RegisterWithShell(localRoot, L"FOO");
        }).wait();

        return S_OK;
}

HRESULT IsSyncRoot(LPCWSTR path, _Out_ bool& isSyncRoot) {
    CF_SYNC_ROOT_BASIC_INFO info = { 0 };
    DWORD returnedLength;
    auto hr = CfGetSyncRootInfoByPath(path, CF_SYNC_ROOT_INFO_CLASS::CF_SYNC_ROOT_INFO_BASIC, &info, sizeof(info), &returnedLength);
    isSyncRoot = hr == S_OK && info.SyncRootFileId.QuadPart != 0;
    return S_OK;

    if (hr == 0x80070186) { // 0x80070186: This operation is only supported in a SyncRoot.
        return S_OK;
    }
    return hr;
}


HRESULT Init(std::wstring localRoot) {
    bool isAlreadySyncRoot;
    auto hr = IsSyncRoot(localRoot.c_str(), isAlreadySyncRoot);
    if (hr != S_OK) {
        return hr;
    }

    if (!isAlreadySyncRoot) {
        hr = Mount(localRoot);
        if (hr != S_OK) {
            return hr;
        }
    }

    ConnectSyncRootTransferCallbacks(localRoot);
    return S_OK;
}

static void CALLBACK OnFetchData(const CF_CALLBACK_INFO* ci, const CF_CALLBACK_PARAMETERS* cp)
{
    std::wcout << "OnFetchData";
    std::wcout << " from:" << ci->ProcessInfo->ImagePath;
    std::wcout << " offset:" << cp->FetchData.RequiredFileOffset.QuadPart;
    std::wcout << " len:" << cp->FetchData.RequiredLength.QuadPart;
    std::wcout << std::endl;

    CF_OPERATION_INFO opInfo = { 0 };
    CF_OPERATION_PARAMETERS opParams = { 0 };

    opInfo.StructSize = sizeof(opInfo);
    opInfo.Type = CF_OPERATION_TYPE_TRANSFER_DATA;
    opInfo.ConnectionKey = ci->ConnectionKey;
    opInfo.TransferKey = ci->TransferKey;

#define FIELD_SIZE( type, field ) ( sizeof( ( (type*)0 )->field ) )
#define CF_SIZE_OF_OP_PARAM( field )                                           \
    ( FIELD_OFFSET( CF_OPERATION_PARAMETERS, field ) +                         \
      FIELD_SIZE( CF_OPERATION_PARAMETERS, field ) )

    opParams.ParamSize = CF_SIZE_OF_OP_PARAM(TransferData);
    opParams.TransferData.CompletionStatus = S_FALSE;
    opParams.TransferData.Buffer = nullptr;
    opParams.TransferData.Offset = cp->FetchData.RequiredFileOffset;
    opParams.TransferData.Length = cp->FetchData.RequiredLength;
    auto hr = CfExecute(&opInfo, &opParams);
    assert(hr == S_OK);

#undef FIELD_SIZE
#undef CF_SIZE_OF_OP_PARAM
}

CF_CALLBACK_REGISTRATION s_MirrorCallbackTable[] =
{
    { CF_CALLBACK_TYPE_FETCH_DATA, OnFetchData },
    CF_CALLBACK_REGISTRATION_END
};

void ConnectSyncRootTransferCallbacks(std::wstring localRoot)
{
    auto hr = CfConnectSyncRoot(
        localRoot.c_str(),
        s_MirrorCallbackTable,
        nullptr,
        CF_CONNECT_FLAGS::CF_CONNECT_FLAG_NONE,
        &s_transferCallbackConnectionKey);
    if (hr != S_OK)
    {
        assert(false);
    }
}

void DisconnectSyncRootTransferCallbacks()
{
    auto hr = CfDisconnectSyncRoot(s_transferCallbackConnectionKey);
    assert(hr == S_OK);
}

HRESULT CreatePlaceHolder(_In_ std::wstring localRoot, _In_ PCWSTR parentPath, _In_ std::wstring fileName, uint64_t size)
{
    std::wstring relativePath(parentPath);
    if (relativePath.size() > 0)
        if (relativePath.at(relativePath.size() - 1) != L'\\')
        {
            relativePath.append(L"\\");
        }
    relativePath.append(fileName);

    FileMetaData metadata = {};

    metadata.FileSize = size;
    metadata.IsDirectory = false;

    fileName.copy(metadata.Name, fileName.length());

    CF_PLACEHOLDER_CREATE_INFO cloudEntry;
    auto fileIdentety = L"F";
    cloudEntry.FileIdentity = &fileIdentety;
    cloudEntry.FileIdentityLength = sizeof(fileIdentety);

    cloudEntry.RelativeFileName = relativePath.data();
    cloudEntry.Flags = CF_PLACEHOLDER_CREATE_FLAGS::CF_PLACEHOLDER_CREATE_FLAG_MARK_IN_SYNC;
    cloudEntry.FsMetadata.FileSize.QuadPart = metadata.FileSize;
    cloudEntry.FsMetadata.BasicInfo.FileAttributes = metadata.FileAttributes;
    cloudEntry.FsMetadata.BasicInfo.CreationTime = metadata.CreationTime;
    cloudEntry.FsMetadata.BasicInfo.LastWriteTime = metadata.LastWriteTime;
    cloudEntry.FsMetadata.BasicInfo.LastAccessTime = metadata.LastAccessTime;
    cloudEntry.FsMetadata.BasicInfo.ChangeTime = metadata.ChangeTime;

    auto hr = CfCreatePlaceholders(localRoot.c_str(), &cloudEntry, 1, CF_CREATE_FLAGS::CF_CREATE_FLAG_NONE, NULL);
    assert(hr == S_OK);

    return cloudEntry.Result;
}

// Copyright 2022 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

/** @file
 * @brief Interface for the Session Manager low level C API.
 */

#pragma once

// GameKit Unreal Plugin
#include <AwsGameKitCore/Public/Core/AwsGameKitLibraryWrapper.h>
#include <AwsGameKitCore/Public/Core/AwsGameKitLibraryUtils.h>
#include <AwsGameKitCore/Public/Core/AwsGameKitMarshalling.h>
#include <AwsGameKitCore/Public/Core/AwsGameKitDispatcher.h>

// GameKit
#if PLATFORM_IOS || PLATFORM_ANDROID
#include <aws/gamekit/authentication/exports.h>
#endif

// Standard library
#include <string>

/**
 * @brief Pointer to an instance of a SessionManager class created in the imported Session Manager C library.
 *
 * Most GameKit C APIs require an instance handle to be passed in.
 *
 * Instance handles are stored as a void* (instead of a class type) because the GameKit C libraries expose a C-level interface (not a C++ interface).
 */
typedef void* GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE;

/**
 * This class exposes the GameKit Session Manager APIs and loads the underlying DLL into memory.
 *
 * This is a barebones wrapper over the DLL's C-level interface. It uses C data types instead of Unreal data types (ex: char* instead of FString).
 */
class AWSGAMEKITRUNTIME_API AwsGameKitSessionManagerWrapper : public AwsGameKitLibraryWrapper
{
private:
    /**
    * Function pointer handles
    */
    DEFINE_FUNC_HANDLE(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE, GameKitSessionManagerInstanceCreate, (const char* clientConfigFile, FuncLogCallback logCb));
    DEFINE_FUNC_HANDLE(bool, GameKitSessionManagerAreSettingsLoaded, (GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, FeatureType featureType));
    DEFINE_FUNC_HANDLE(void, GameKitSessionManagerReloadConfigFile, (GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, const char* clientConfigFile));
    DEFINE_FUNC_HANDLE(void, GameKitSessionManagerReloadConfigContents, (GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, const char* clientConfigFileContents));
    DEFINE_FUNC_HANDLE(void, GameKitSessionManagerSetToken, (GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, GameKit::TokenType tokenType, const char* value));
    DEFINE_FUNC_HANDLE(void, GameKitSessionManagerInstanceRelease, (GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance));

protected:
    virtual std::string getLibraryFilename() override
    {
#if PLATFORM_WINDOWS
        return "aws-gamekit-authentication";
#elif PLATFORM_MAC
        return "libaws-gamekit-authentication";
#else
        return "";
#endif
    }

    virtual void importFunctions(void* loadedDllHandle) override;

public:
    AwsGameKitSessionManagerWrapper() {};
    virtual ~AwsGameKitSessionManagerWrapper() {};

    /**
     * @brief Create a GameKitSessionManager instance, which can be used to access the SessionManager API.
     *
     * @details Make sure to call GameKitAccountInstanceRelease() to destroy the returned object when finished with it.
     *
     * @param clientConfigFile (Optional, can be a nullptr or empty string) Relative filepath to the generated file "awsGameKitClientConfig.yml".
     * The config file is generated by GameKit each time a feature is deployed or re-deployed, and has settings for each GameKit feature you've deployed.
     * @param logCb Callback function for logging information and errors.
     * @return Pointer to the new GameKitSessionManager instance.
     */
    virtual GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE GameKitSessionManagerInstanceCreate(const char* clientConfigFile, FuncLogCallback logCb);

    /**
     * @brief Destroy the provided GameKitSessionManager instance.
     *
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
     */
    virtual void GameKitSessionManagerInstanceRelease(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance);

    /**
     * @brief Check if the settings are loaded for the feature.
     *
     * @detailed These settings are found in file "awsGameKitClientConfig.yml" which is generated by GameKit each time you deploy or re-deploy a feature.
     * The file is loaded by calling either GameKitSessionManagerInstanceCreate(), GameKitSessionManagerReloadConfigFile(), or ReloadConfig().
     *
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
     * @param featureType The feature to check.
     * @return True if the settings for the feature are loaded, false otherwise.
    */
    virtual bool GameKitSessionManagerAreSettingsLoaded(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, FeatureType featureType);

    /**
     * @brief Replace any loaded client settings with new settings from the provided file.
     *
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
     * @param clientConfigFile Relative filepath to the generated file "awsGameKitClientConfig.yml".
     * The config file is generated by GameKit each time a feature is deployed or re-deployed, and has settings for each GameKit feature you've deployed.
     */
    virtual void GameKitSessionManagerReloadConfigFile(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, const char* clientConfigFile);

    /**
     * @brief Replace any loaded client settings with new settings from the provided file.
     *
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
     * @param clientConfigFileContents Contents of "awsGameKitClientConfig.yml".
     * The config file is generated by GAMEKIT each time a feature is deployed or re-deployed, and has settings for each GAMEKIT feature you've deployed.
     */
    virtual void GameKitSessionManagerReloadConfigContents(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, const char* clientConfigFileContents);

    /**
     * @brief Same as GameKitSessionManagerReloadConfigFile(), except the file path is determined automatically.
     *
     * @details The "awsGameKitClientConfig.yml" is recursively searched for in these root locations:
     * In editor mode - one level above FPaths::GameSourceDir().
     * In non-editor mode - FPaths::LaunchDir().
     *
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
    */
    virtual void ReloadConfig(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance);

#if UE_BUILD_DEVELOPMENT && WITH_EDITOR
    /*
     * @brief Loads an environment-specific config and copies it on disk so that it can be loaded
     * with subsequent ReloadConfig() calls
     *
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
     * @param subfolder FString to the subfolder that contains the environment specific config to load.
    */
    virtual void ReloadConfig(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, const FString& subfolder);
#endif

    /**
     * @brief Sets a token's value.
     * @param sessionManagerInstance Pointer to GameKitSessionManager instance created with GameKitSessionManagerInstanceCreate().
     * @param tokenType The type of token to set.
     * @param value The value of the token.
    */
    virtual void GameKitSessionManagerSetToken(GAMEKIT_SESSION_MANAGER_INSTANCE_HANDLE sessionManagerInstance, GameKit::TokenType tokenType, const char* value);
};

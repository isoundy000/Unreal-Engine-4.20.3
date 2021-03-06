// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IBuildPatchServicesModule.h"
#include "BuildPatchSettings.h"

class BUILDPATCHSERVICES_API FBuildPatchServices
{
public:
	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(ModuleName);
	}

	static IBuildPatchServicesModule& Get()
	{
		return FModuleManager::Get().GetModuleChecked<IBuildPatchServicesModule>(ModuleName);
	}

	static const BuildPatchServices::FBuildPatchServicesInitSettings& GetSettings()
	{
		return InitSettings;
	}

	static void Set(
		const FName& Value, 
		const BuildPatchServices::FBuildPatchServicesInitSettings& BuildPatchServicesInitSettings = BuildPatchServices::FBuildPatchServicesInitSettings())
	{
		if(IsAvailable())
		{
			Shutdown();
		}
		ModuleName = Value;
		InitSettings = BuildPatchServicesInitSettings;
		FModuleManager::Get().LoadModuleChecked<IBuildPatchServicesModule>(ModuleName);
	}

	static void Shutdown()
	{
		if(IsAvailable())
		{
			FModuleManager::Get().UnloadModule(ModuleName);
		}
	}

private:

	static FName ModuleName;
	static BuildPatchServices::FBuildPatchServicesInitSettings InitSettings;
};

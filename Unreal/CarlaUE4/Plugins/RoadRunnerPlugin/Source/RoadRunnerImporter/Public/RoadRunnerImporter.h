// Copyright 2018 VectorZero, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include <UnrealEd.h>


////////////////////////////////////////////////////////////////////////////////
// Imports FBX files with RoadRunner metadata
//	- Parses metadata XML lookaside file to set material properties and other attributes
//	- Material instances are created from the base materials located in the plugin's content folder
//  - Sets up signal components after importing
class FRoadRunnerImporterModule : public IModuleInterface
{
public:
	// Asset processing delegates
	static void RoadRunnerPostProcessing(UFactory* inFactory, UObject* inCreateObject);

	// Public helpers
	static ANSICHAR* MakeName(const ANSICHAR* Name);

	// IModuleInterface implementation
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static int CurrentMetadataVersion;
	static const int PluginVersion = 1;
	static const int32 TransparentRenderQueue = 1000;
};

// Copyright 2018 VectorZero, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealEd.h"
#include "Factories.h"
#include "UObject/ObjectMacros.h"
#include "Factories/Factory.h"
#include "Factories/FbxSceneImportFactory.h"
#include "RoadRunnerFbxSceneImportFactory.generated.h"

////////////////////////////////////////////////////////////////////////////////
// Import with UFbxSceneImportFactory using the pre/post asset import delegates
// defined in RoadRunnerImporter.
UCLASS(hidecategories = Object)
class ROADRUNNERIMPORTER_API URoadRunnerFbxSceneImportFactory : public UFbxSceneImportFactory
{
	GENERATED_UCLASS_BODY()

	// UFactory Interface
	virtual UObject* FactoryCreateFile(UClass* inClass, UObject* inParent, FName inName, EObjectFlags flags, const FString& filename, const TCHAR* parms, FFeedbackContext* warn, bool& bOutOperationCanceled) override;
	virtual bool FactoryCanImport(const FString& filename) override;
};

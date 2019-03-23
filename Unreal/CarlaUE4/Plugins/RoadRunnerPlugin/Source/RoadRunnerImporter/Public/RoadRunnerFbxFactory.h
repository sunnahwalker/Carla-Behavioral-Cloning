// Copyright 2018 VectorZero, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealEd.h"
#include "Factories.h"
#include "UObject/ObjectMacros.h"
#include "Factories/Factory.h"
#include "Factories/FbxFactory.h"
#include "RoadRunnerFbxFactory.generated.h"

////////////////////////////////////////////////////////////////////////////////
// Attempts to import using the custom RoadRunner scene import factory.
// If it fails, go back to the default fbx importer.
// Note: This factory is needed to overwrite the normal fbx import with
// our scene importer since Factory priority doesn't work with FbxSceneImportFactory
UCLASS(hidecategories = Object)
class ROADRUNNERIMPORTER_API URoadRunnerFbxFactory : public UFbxFactory
{
	GENERATED_UCLASS_BODY()

	// UFactory Interface
	virtual UObject* FactoryCreateFile(UClass* inClass, UObject* inParent, FName inName, EObjectFlags flags, const FString& filename, const TCHAR* parms, FFeedbackContext* warn, bool& bOutOperationCanceled) override;
	virtual bool FactoryCanImport(const FString& filename) override;
};

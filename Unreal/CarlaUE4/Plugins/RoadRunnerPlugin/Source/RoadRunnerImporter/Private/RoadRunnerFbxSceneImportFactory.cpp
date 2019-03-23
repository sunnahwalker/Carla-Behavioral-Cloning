// Copyright 2018 VectorZero, Inc. All Rights Reserved.

#include "RoadRunnerFbxSceneImportFactory.h"
#include "RoadRunnerImporterLog.h"
#include "RoadRunnerImporter.h"

#include <Factories/Factory.h>
#include <Editor/UnrealEd/Public/Editor.h>
#include <UnrealEd.h>
#include <Runtime/Engine/Classes/PhysicsEngine/BodySetup.h>
#include <ObjectTools.h>
#include <PackageTools.h>

#define LOCTEXT_NAMESPACE "RoadRunnerFBXSceneImportFactory"

////////////////////////////////////////////////////////////////////////////////
// Note: The "Import Into Level" button does not currently check for priority,
// so this factory will only be called through RoadRunnerFbxFactory.
URoadRunnerFbxSceneImportFactory::URoadRunnerFbxSceneImportFactory(const FObjectInitializer& objectInitializer) : Super(objectInitializer)
{
	SupportedClass = UBlueprint::StaticClass();
	Formats.Add(TEXT("fbx;FBX meshes and animations"));
	ImportPriority = DefaultImportPriority + 1;
}

////////////////////////////////////////////////////////////////////////////////
// Checks for metadata file, and runs our pre/post asset import functions
// if it exists.
UObject * URoadRunnerFbxSceneImportFactory::FactoryCreateFile(UClass * inClass, UObject * inParent, FName inName, EObjectFlags flags, const FString & filename, const TCHAR * parms, FFeedbackContext * warn, bool & bOutOperationCanceled)
{
	FString rrMetadataFile = FPaths::ChangeExtension(filename, ".rrdata.xml");

	// Only use our delegates if metadata file exists, and is valid.
	if (!FPaths::FileExists(rrMetadataFile))
	{
		return UFbxSceneImportFactory::FactoryCreateFile(inClass, inParent, inName, flags, filename, parms, warn, bOutOperationCanceled);
	}

	FXmlFile* rrXml = new FXmlFile(rrMetadataFile);
	if (!rrXml->IsValid())
	{
		UE_LOG(RoadRunnerImporter, Warning, TEXT("RoadRunner metadata file not valid. Reverting to default scene import factory."));
		return UFbxSceneImportFactory::FactoryCreateFile(inClass, inParent, inName, flags, filename, parms, warn, bOutOperationCanceled);
	}

	FString FileExtension = FPaths::GetExtension(filename);

	// Disable LogFbx
	GEngine->Exec(GetWorld(), TEXT("Log LogFbx off"));

	// Import materials and model
	UObject* newObj = UFbxSceneImportFactory::FactoryCreateFile(inClass, inParent, inName, flags, filename, parms, warn, bOutOperationCanceled);

	// Reset log levels
	GEngine->Exec(GetWorld(), TEXT("Log reset"));

	// Set up signals
	FEditorDelegates::OnAssetPostImport.Broadcast(this, newObj);

	FString packagePath = FPaths::GetPath(inParent->GetName()) + "/";

	// Use complex collisions and fix material references
	for (auto ItAsset = AllNewAssets.CreateIterator(); ItAsset; ++ItAsset)
	{
		UObject *AssetObject = ItAsset.Value();
		if (AssetObject)
		{
			if (AssetObject->IsA(UStaticMesh::StaticClass()))
			{
				//change to complex collisions
				UStaticMesh* staticMesh = Cast<UStaticMesh>(AssetObject);
				staticMesh->BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;

				for (auto& matref : staticMesh->StaticMaterials)
				{
					// Re-create material package name from old material reference
					FString materialFullName = matref.ImportedMaterialSlotName.ToString();
					// Follow Unreal's naming scheme
					materialFullName = UTF8_TO_TCHAR(FRoadRunnerImporterModule::MakeName(TCHAR_TO_ANSI(*materialFullName)));
					materialFullName = ObjectTools::SanitizeObjectName(materialFullName);

					FString basePackageName = FPackageName::GetLongPackagePath(packagePath) / materialFullName;
					basePackageName = PackageTools::SanitizePackageName(basePackageName);

					matref.MaterialInterface = LoadObject<UMaterialInterface>(nullptr, *basePackageName);
				}

				staticMesh->PreEditChange(NULL);
				staticMesh->PostEditChange();
			}
		}
	}

	return newObj;
}

////////////////////////////////////////////////////////////////////////////////

bool URoadRunnerFbxSceneImportFactory::FactoryCanImport(const FString& filename)
{
	const FString extension = FPaths::GetExtension(filename);

	if (extension == TEXT("fbx"))
	{
		return true;
	}
	return false;
}

#undef LOCTEXT_NAMESPACE
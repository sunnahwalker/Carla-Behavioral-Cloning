// Copyright 2018 VectorZero, Inc. All Rights Reserved.

#include "RoadRunnerFbxFactory.h"
#include "RoadRunnerImporterLog.h"
#include "RoadRunnerFbxSceneImportFactory.h"

#include <Factories/Factory.h>
#include <Editor/UnrealEd/Public/Editor.h>
#include <Factories/FbxFactory.h>
#include <UnrealEd.h>
#include <Runtime/XmlParser/Public/XmlFile.h>

////////////////////////////////////////////////////////////////////////////////
// Sets a higher import priority so this factory will be used when trying to
// import fbx files.
URoadRunnerFbxFactory::URoadRunnerFbxFactory(const FObjectInitializer& objectInitializer) : Super(objectInitializer)
{
	SupportedClass = UBlueprint::StaticClass();
	Formats.Add(TEXT("fbx;FBX meshes and animations"));
	ImportPriority = DefaultImportPriority + 1;
}

////////////////////////////////////////////////////////////////////////////////
// Try to create the object using the RoadRunner scene import factory,
// otherwise, fall back to the default.
UObject * URoadRunnerFbxFactory::FactoryCreateFile(UClass * inClass, UObject * inParent, FName inName, EObjectFlags flags, const FString & filename, const TCHAR * parms, FFeedbackContext * warn, bool & bOutOperationCanceled)
{
	FString rrMetadataFile = FPaths::ChangeExtension(filename, ".rrdata.xml");

	// Only use our scene import if metadata file exists, and is valid, otherwise use the default one
	if (!FPaths::FileExists(rrMetadataFile))
	{
		return UFbxFactory::FactoryCreateFile(inClass, inParent, inName, flags, filename, parms, warn, bOutOperationCanceled);
	}

	FXmlFile* rrXml = new FXmlFile(rrMetadataFile);
	if (!rrXml->IsValid())
	{
		UE_LOG(RoadRunnerImporter, Warning, TEXT("RoadRunner metadata file not valid. Reverting to default fbx factory."));
		return UFbxFactory::FactoryCreateFile(inClass, inParent, inName, flags, filename, parms, warn, bOutOperationCanceled);
	}

	auto fbxSceneFactory = NewObject<URoadRunnerFbxSceneImportFactory>();

	// Protect factory from garbage collection
	fbxSceneFactory->AddToRoot();
	UObject* newObj = fbxSceneFactory->FactoryCreateFile(inClass, inParent, inName, flags, filename, parms, warn, bOutOperationCanceled);
	fbxSceneFactory->RemoveFromRoot();

	return newObj;
}

////////////////////////////////////////////////////////////////////////////////

bool URoadRunnerFbxFactory::FactoryCanImport(const FString& filename)
{
	const FString extension = FPaths::GetExtension(filename);

	if (extension == TEXT("fbx"))
	{
		return true;
	}
	return false;
}
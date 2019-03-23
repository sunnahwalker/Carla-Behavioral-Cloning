// Copyright 2018 VectorZero, Inc. All Rights Reserved.

#include "RoadRunnerImporter.h"
#include "RoadRunnerImporterLog.h"
#include "RoadRunnerTrafficJunction.h"
#include "RoadRunnerFbxSceneImportFactory.h"

#include <AssetRegistryModule.h>
#include <Developer/AssetTools/Public/AssetToolsModule.h>
#include <Developer/AssetTools/Public/IAssetTools.h>
#include <Editor/MaterialEditor/Public/IMaterialEditor.h>
#include <Editor/UnrealEd/Public/Editor.h>
#include <Editor/UnrealEd/Public/Kismet2/KismetEditorUtilities.h>
#include <Editor/UnrealEd/Public/Layers/ILayers.h>
#include <Factories/Factory.h>
#include <Factories/FbxFactory.h>
#include <unordered_map>
#include <ObjectTools.h>
#include <PackageTools.h>
#include <Runtime/AssetRegistry/Public/ARFilter.h>
#include <Runtime/Core/Public/Internationalization/Regex.h>
#include <Runtime/Launch/Resources/Version.h>
#include <Runtime/XmlParser/Public/XmlFile.h>
#include <UnrealEd.h>
#include <UObject/GCObjectScopeGuard.h>


#define LOCTEXT_NAMESPACE "FRoadRunnerImporterModule"

int FRoadRunnerImporterModule::CurrentMetadataVersion;

namespace
{
	static const FString RoadRunnerExtension = ".rrdata.xml";

	struct FStringHash
	{
		std::size_t operator()(const FString& k) const
		{
			return GetTypeHash(k);
		}
	};

	// Holds strings from the road runner metadata file
	struct MaterialInfo
	{
		FString Name;
		FString DiffuseMap;
		FString NormalMap;
		FString SpecularMap;
		FString DiffuseColor;
		FString SpecularColor;
		FString SpecularFactor;
		FString TransparencyMap;
		FString TransparencyFactor;
		FString Roughness;
		FString Emission;
		FString TextureScaleU;
		FString TextureScaleV;
		FString TwoSided;
		FString DrawQueue;
		FString ShadowCaster;
		FString IsDecal;
	};

	// Material helpers not accessible until 4.17
	// Copied from FbxMaterialImport.cpp:825 (ver 4.15)
#if ENGINE_MINOR_VERSION <= 16
	UMaterialInterface* FindExistingUnrealMaterial(const FString& BasePath, const FString& MaterialName)
	{
		UMaterialInterface* Material = nullptr;

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		TArray<FAssetData> AssetData;
		FARFilter Filter;

		AssetRegistry.SearchAllAssets(true);

		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		Filter.ClassNames.Add(UMaterialInterface::StaticClass()->GetFName());
		Filter.PackagePaths.Add(FName(*BasePath));

		AssetRegistry.GetAssets(Filter, AssetData);

		TArray<UMaterialInterface*> FoundAssets;
		for (const FAssetData& Data : AssetData)
		{
			if (Data.AssetName == FName(*MaterialName))
			{
				Material = Cast<UMaterialInterface>(Data.GetAsset());
				if (Material != nullptr)
				{
					FoundAssets.Add(Material);
				}
			}
		}

		if (FoundAssets.Num() > 1)
		{
			check(Material != nullptr);
			UE_LOG(RoadRunnerImporter, Warning, TEXT("Found multiple materials named %s at %s"), *MaterialName, *BasePath);
		}
		return Material;
	}
#endif

	////////////////////////////////////////////////////////////////////////////////
	// Get the modifed light bulb name by its original name under a given scene
	// component node.
	FString FindByNamePrefix(USCS_Node* parent, FString prefix)
	{
		for (const auto& child : parent->GetChildNodes())
		{
			FString componentName = child->GetVariableName().ToString();
			// Chop off "Node" substring at the end
			int index = componentName.Find(TEXT("Node"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);

			FString baseNodeName = componentName.Left(index);

			if (baseNodeName.Compare(prefix) == 0)
			{
				return componentName;
			}

			FString childCheck = FindByNamePrefix(child, prefix);

			if (!childCheck.IsEmpty())
			{
				return childCheck;
			}
		}
		return TEXT("");
	}

	////////////////////////////////////////////////////////////////////////////////
	// Based off FbxMaterialImport.cpp:37 on version 4.20
	// Significant modifications in logic are commented
	// Creates a UTexture object from the file location and the package destination
	UTexture* ImportTexture(FString absFilePath, FString packagePath, bool setupAsNormalMap)
	{
		if (absFilePath.IsEmpty())
		{
			return nullptr;
		}

		// Create an unreal texture asset
		UTexture2D* unrealTexture = nullptr;
		FString extension = FPaths::GetExtension(absFilePath).ToLower();

		// Name the texture with file name
		FString textureName = FPaths::GetBaseFilename(absFilePath);
		textureName = ObjectTools::SanitizeObjectName(textureName);

		// Set where to place the texture in the project
		FString basePackageName = FPackageName::GetLongPackagePath(packagePath) / textureName;
		basePackageName = PackageTools::SanitizePackageName(basePackageName);

		UTexture2D* existingTexture = nullptr;
		UPackage* texturePackage = nullptr;

		// First check if the asset already exists.
		FString objectPath = basePackageName + TEXT(".") + textureName;
		existingTexture = LoadObject<UTexture2D>(NULL, *objectPath, nullptr, LOAD_Quiet | LOAD_NoWarn);

		// Modified: return existing texture if found instead of updating
		if (existingTexture)
		{
			return existingTexture;
		}

		const FString suffix(TEXT(""));
		// Create new texture asset
		FAssetToolsModule& assetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		FString finalPackageName;
		assetToolsModule.Get().CreateUniqueAssetName(basePackageName, suffix, finalPackageName, textureName);

		texturePackage = CreatePackage(NULL, *finalPackageName);

		// Modified: only use absolute file path since we don't deal with the uncertainty of fbx
		if (!IFileManager::Get().FileExists(*absFilePath))
		{
			UE_LOG(RoadRunnerImporter, Warning, TEXT("Unable to find Texture file %s"), *absFilePath);
			return nullptr;
		}

		bool fileReadSuccess = false;
		TArray<uint8> dataBinary;
		if (!absFilePath.IsEmpty())
		{
			fileReadSuccess = FFileHelper::LoadFileToArray(dataBinary, *absFilePath);
		}

		if (!fileReadSuccess || dataBinary.Num() <= 0)
		{
			UE_LOG(RoadRunnerImporter, Warning, TEXT("Unable to load Texture file %s"), *absFilePath);
			return nullptr;
		}

		UE_LOG(RoadRunnerImporter, Verbose, TEXT("Loading texture file %s"), *absFilePath);
		const uint8* textureData = dataBinary.GetData();
		// Modified: use scope guard for the factory (ver 4.15+) to avoid garbage collection
		UTextureFactory* textureFactory;
		FGCObjectScopeGuard(textureFactory = NewObject<UTextureFactory>());

		// Always re-import
		textureFactory->SuppressImportOverwriteDialog();
		const TCHAR* textureType = *extension;

		// Unless the normal map setting is used during import, 
		// the user has to manually hit "reimport" then "recompress now" button
		if (setupAsNormalMap)
		{
			if (!existingTexture)
			{
				textureFactory->LODGroup = TEXTUREGROUP_WorldNormalMap;
				textureFactory->CompressionSettings = TC_Normalmap;
				// Modified: removed import options
			}
			else
			{
				UE_LOG(RoadRunnerImporter, Warning, TEXT("Manual texture reimport and recompression may be needed for %s"), *textureName);
			}
		}

		unrealTexture = (UTexture2D*)textureFactory->FactoryCreateBinary(
			UTexture2D::StaticClass(), texturePackage, *textureName,
			RF_Standalone | RF_Public, NULL, textureType,
			textureData, textureData + dataBinary.Num(), GWarn);

		if (unrealTexture != NULL)
		{
			// Modified: Always sample as linear color
			if (setupAsNormalMap)
			{
				unrealTexture->SRGB = 0;
			}
			else
			{
				unrealTexture->SRGB = 1;
			}

			// Make sure the AssetImportData point on the texture file and not on the fbx files since the factory point on the fbx file
			unrealTexture->AssetImportData->Update(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*absFilePath));

			// Notify the asset registry
			FAssetRegistryModule::AssetCreated(unrealTexture);
			// Set the dirty flag so this package will get saved later
			texturePackage->SetDirtyFlag(true);
			texturePackage->PostEditChange();
		}
		else
		{
			UE_LOG(RoadRunnerImporter, Error, TEXT("Texture %s could not be created."), *textureName);
		}

		return unrealTexture;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Helper function to set texture parameter in a material instance
	void SetTextureParameter(UMaterialInstanceConstant* material, const FName& paramName, const FString& baseFilePath, const FString& texturePath, const FString& packagePath, bool isNormal)
	{
		if (texturePath.IsEmpty())
			return;

		FString texFileAbsPath = FPaths::ConvertRelativePathToFull(baseFilePath / texturePath);
		UTexture * texture = ImportTexture(texFileAbsPath, packagePath, isNormal);
		if (texture)
		{
#if ENGINE_MINOR_VERSION > 18
			material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(paramName, EMaterialParameterAssociation::GlobalParameter), texture);
#else
			material->SetTextureParameterValueEditorOnly(paramName, texture);
#endif
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Helper function to set color parameter in a material instance
	void SetColorParameter(UMaterialInstanceConstant* material, const FName& paramName, const FString& colorString, float alphaVal)
	{
		if (colorString.IsEmpty())
		{
			return;
		}
		
		TArray<FString> colorStrings;
		int numElements = colorString.ParseIntoArray(colorStrings, TEXT(","), true);
		if (numElements != 3)
		{
			UE_LOG(RoadRunnerImporter, Error, TEXT("Error: %s's %s value is invalid"), *(material->GetFName().ToString()), *(paramName.ToString()));
			return;
		}
		float r = FCString::Atof(*(colorStrings[0]));
		float g = FCString::Atof(*(colorStrings[1]));
		float b = FCString::Atof(*(colorStrings[2]));
#if ENGINE_MINOR_VERSION > 18
		material->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(paramName, EMaterialParameterAssociation::GlobalParameter), FLinearColor(r, g, b, alphaVal));
#else
		material->SetVectorParameterValueEditorOnly(paramName, FLinearColor(r, g, b, alphaVal));
#endif
	
	}

	////////////////////////////////////////////////////////////////////////////////
	// Helper function to set scalar parameter in a material instance
	void SetScalarParameter(UMaterialInstanceConstant* material, const FName& paramName, const FString& valueString)
	{
		if (valueString.IsEmpty())
			return;

		float value = FCString::Atof(*valueString);
#if ENGINE_MINOR_VERSION > 18
		material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(paramName, EMaterialParameterAssociation::GlobalParameter), value);
#else
		material->SetScalarParameterValueEditorOnly(paramName, value);
#endif
	
	}
	

	////////////////////////////////////////////////////////////////////////////////
	// Based off FbxMaterialImport.cpp:502 on version 4.20
	// Parses the material info and creates material instance assets from the base materials included with the plugin
	void CreateUnrealMaterial(FString sourceFilePath, FString packagePath, MaterialInfo materialInfo, TMap<FString, int32>& materialToLayerMap)
	{
		FString materialFullName = materialInfo.Name;
		FString basePackageName = FPackageName::GetLongPackagePath(packagePath) / materialFullName;
		basePackageName = PackageTools::SanitizePackageName(basePackageName);

		// Create new package
		const FString Suffix(TEXT(""));

		// Get the original material package and delete it
		UMaterial* oldMat = LoadObject<UMaterial>(nullptr, *basePackageName);
		if (oldMat != nullptr)
		{
			UPackage* oldPackage = CreatePackage(NULL, *basePackageName);

			// Notify the asset registry
			FAssetRegistryModule::AssetDeleted(oldMat);

			// Static function for PackageDeleted not added until 4.17
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(AssetRegistryConstants::ModuleName);
			AssetRegistryModule.Get().PackageDeleted(oldPackage);

			oldMat->MarkPendingKill();
			oldPackage->MarkPendingKill();

			oldMat->ConditionalBeginDestroy();
			oldPackage->ConditionalBeginDestroy();
		}

		// Re-create package for new material instance
		UPackage* package = CreatePackage(NULL, *basePackageName);

		// Find our base material to instance from
		FText materialSearchError;
		UMaterialInterface* baseMaterial = nullptr;
		float alphaVal = 1.0f; // Default to opaque
		float transparency = 0.0f;
		if (!materialInfo.TransparencyFactor.IsEmpty())
		{
			transparency = FCString::Atof(*(materialInfo.TransparencyFactor));
		}
		bool isTransparent = !materialInfo.TransparencyMap.IsEmpty() || transparency > 0;


		FString materialName = "BaseMaterial";
		alphaVal = 1.0f - transparency;
		// Version 1 adds DrawQueue and ShadowCaster fields
		if (FRoadRunnerImporterModule::CurrentMetadataVersion >= 1)
		{
			// Choose base material based off transparency
			if (isTransparent)
			{
				if (materialInfo.TwoSided.Equals("true"))
				{
					// Markings are always rendered with translucent blend mode
					if (!materialInfo.DrawQueue.Equals("0") || materialInfo.ShadowCaster.Equals("false"))
					{
						// Translucent blend mode
						materialName = "BaseTransparentMaterialTwoSided";
					}
					else
					{
						// Masked blend mode
						materialName = "BaseCutoutMaterialTwoSided";
					}
				}
				else
				{
					// Markings are always rendered with translucent blend mode
					if (!materialInfo.DrawQueue.Equals("0") || materialInfo.ShadowCaster.Equals("false"))
					{
						// Translucent blend mode
						materialName = "BaseTransparentMaterial";
					}
					else
					{
						// Masked blend mode
						materialName = "BaseCutoutMaterial";
					}
				}
			}
		}
		else
		{
			// Choose base material based off transparency
			if (isTransparent)
			{
				if (materialInfo.TwoSided.Equals("true"))
				{
					if (transparency > 0.0f)
					{
						// Translucent blend mode
						materialName = "BaseTransparentMaterialTwoSided";
					}
					else
					{
						// Masked blend mode
						materialName = "BaseCutoutMaterialTwoSided";
					}
				}
				else
				{
					if (transparency > 0.0f)
					{
						// Translucent blend mode
						materialName = "BaseTransparentMaterial";
					}
					else
					{
						// Masked blend mode
						materialName = "BaseCutoutMaterial";
					}
				}
			}
		}
		
#if ENGINE_MINOR_VERSION <= 16
		baseMaterial = FindExistingUnrealMaterial("/RoadRunnerImporter", materialName);
#else
		baseMaterial = UMaterialImportHelpers::FindExistingMaterialFromSearchLocation(materialName, "/RoadRunnerImporter/", EMaterialSearchLocation::UnderParent, materialSearchError);
#endif
		if (!materialSearchError.IsEmpty() || baseMaterial == nullptr)
		{
			UE_LOG(RoadRunnerImporter, Error, TEXT("Base material not found: %s"), *(materialSearchError.ToString()));
			return;
		}

		// Create material instance from our base material
		// Modified: always create a material instance from our base material
		auto materialInstanceFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
		materialInstanceFactory->InitialParent = baseMaterial;
		UMaterialInstanceConstant* unrealMaterial = (UMaterialInstanceConstant*)materialInstanceFactory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(), package, *materialFullName, RF_Standalone | RF_Public, NULL, GWarn);
		if (unrealMaterial == NULL)
		{
			UE_LOG(RoadRunnerImporter, Error, TEXT("Material %s could not be created."), *materialInfo.Name);
			return;
		}

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(unrealMaterial);
		// Set the dirty flag so this package will get saved later
		package->SetDirtyFlag(true);

		// Set parameters
		// Modified: set parameters based off imported material info
		SetTextureParameter(unrealMaterial, FName(TEXT("DiffuseMap")), sourceFilePath, materialInfo.DiffuseMap, packagePath, false);
		SetTextureParameter(unrealMaterial, FName(TEXT("SpecularMap")), sourceFilePath, materialInfo.SpecularMap, packagePath, false);
		SetTextureParameter(unrealMaterial, FName(TEXT("NormalMap")), sourceFilePath, materialInfo.NormalMap, packagePath, true);
		
		SetColorParameter(unrealMaterial, FName(TEXT("DiffuseColor")), materialInfo.DiffuseColor, alphaVal);
		SetColorParameter(unrealMaterial, FName(TEXT("SpecularColor")), materialInfo.SpecularColor, 1);

		SetScalarParameter(unrealMaterial, FName(TEXT("SpecularFactor")), materialInfo.SpecularFactor);
		SetScalarParameter(unrealMaterial, FName(TEXT("Roughness")), materialInfo.Roughness);
		SetScalarParameter(unrealMaterial, FName(TEXT("Emission")), materialInfo.Emission);
		SetScalarParameter(unrealMaterial, FName(TEXT("ScalingU")), materialInfo.TextureScaleU);
		SetScalarParameter(unrealMaterial, FName(TEXT("ScalingV")), materialInfo.TextureScaleV);

		// let the material update itself if necessary
		unrealMaterial->PreEditChange(NULL);
		unrealMaterial->PostEditChange();

		int32 adjustedDrawQueue = FCString::Atoi(*materialInfo.DrawQueue);
		if (isTransparent && materialInfo.IsDecal.Equals("false"))
		{
			// Render other transparent objects after decals
			adjustedDrawQueue = FRoadRunnerImporterModule::TransparentRenderQueue;
		}
		materialToLayerMap.Add(materialFullName, adjustedDrawQueue);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "LightState" element. Contains "Name" and "State".
	FLightBulbState LoadLightBulbState(FXmlNode* lightStateNode)
	{
		FLightBulbState lightBulbState;
		for (const auto& lightStateProperty : lightStateNode->GetChildrenNodes())
		{
			const FString& tag = lightStateProperty->GetTag();
			if (tag.Equals(TEXT("Name"), ESearchCase::CaseSensitive))
			{
				lightBulbState.Name = lightStateProperty->GetContent();
			}
			else if (tag.Equals(TEXT("State"), ESearchCase::CaseSensitive))
			{
				FString stateString = lightStateProperty->GetContent();
				lightBulbState.State = stateString.Equals(TEXT("true"), ESearchCase::CaseSensitive);
			}

		}
		return lightBulbState;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "Configuration" element. Contains "Name" and multiple
	// "LightState" elements.
	FSignalConfiguration LoadSignalConfiguration(FXmlNode* configurationNode)
	{
		FSignalConfiguration signalConfiguration;
		for (const auto& signalConfigurationProperty : configurationNode->GetChildrenNodes())
		{
			const FString& tag = signalConfigurationProperty->GetTag();
			if (tag.Equals(TEXT("Name"), ESearchCase::CaseSensitive))
			{
				signalConfiguration.Name = signalConfigurationProperty->GetContent();
			}
			else if (tag.Equals(TEXT("LightState"), ESearchCase::CaseSensitive))
			{
				FLightBulbState lightState = LoadLightBulbState(signalConfigurationProperty);
				signalConfiguration.LightBulbStates.Add(lightState);
			}
		}

		return signalConfiguration;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "Signal" asset element. Contains "ID" and multiple
	// "Configuration" elements.
	FSignalAsset LoadSignalAsset(FXmlNode* signalNode)
	{
		FSignalAsset signalAsset;
		for (const auto& signalAssetProperty : signalNode->GetChildrenNodes())
		{
			const FString& tag = signalAssetProperty->GetTag();
			if (tag.Equals(TEXT("ID"), ESearchCase::CaseSensitive))
			{
				signalAsset.Id = signalAssetProperty->GetContent();
			}
			else if (tag.Equals(TEXT("Configuration"), ESearchCase::CaseSensitive))
			{
				FSignalConfiguration signalConfiguration = LoadSignalConfiguration(signalAssetProperty);
				signalAsset.SignalConfigurations.Add(signalConfiguration);
			}
		}
		return signalAsset;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Load the signal assets into the array.
	void LoadSignalAssets(FXmlNode* signalDataNode, std::unordered_map<FString, FSignalAsset, FStringHash>& outUuidToSignalAssetMap)
	{
		auto signalAssetsNode = signalDataNode->FindChildNode(TEXT("SignalAssets"));

		if (!signalAssetsNode)
		{
			return;
		}

		for (const auto& signalAssetsProperty : signalAssetsNode->GetChildrenNodes())
		{
			const FString& tag = signalAssetsProperty->GetTag();
			if (tag.Equals(TEXT("Signal"), ESearchCase::CaseSensitive))
			{
				FSignalAsset signalAsset = LoadSignalAsset(signalAssetsProperty);
				outUuidToSignalAssetMap[signalAsset.Id] = signalAsset;
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "Signal" state element. Contains the "ID" of the signal, the
	// "SignalAsset" ID defined in SignalAssets, and the "State" for which
	// configuration it is currently in.
	FSignalState LoadSignalState(FXmlNode* signalStateNode, const std::unordered_map<FString, USCS_Node*, FStringHash>& uuidToComponentMap, const std::unordered_map<FString, FSignalAsset, FStringHash>& uuidToSignalAssetMap)
	{
		FSignalState signalState;
		for (const auto& signalStateProperty : signalStateNode->GetChildrenNodes())
		{
			const FString& tag = signalStateProperty->GetTag();
			if (tag.Equals(TEXT("ID"), ESearchCase::CaseSensitive))
			{
				signalState.Id = signalStateProperty->GetContent();
			}
			else if (tag.Equals(TEXT("SignalAsset"), ESearchCase::CaseSensitive))
			{
				signalState.SignalAssetId = signalStateProperty->GetContent();
			}
			else if (tag.Equals(TEXT("ConfigurationIndex"), ESearchCase::CaseSensitive))
			{
				signalState.Configuration = FCString::Atoi(*signalStateProperty->GetContent());
			}
		}

		// Find the signal configuration by the id
		if (uuidToSignalAssetMap.count(signalState.SignalAssetId) == 0)
		{
			UE_LOG(RoadRunnerImporter, Warning, TEXT("Signal Asset %s could not be found."), *signalState.SignalAssetId);
			return signalState;
		}
		const FSignalAsset& signalAsset = uuidToSignalAssetMap.at(signalState.SignalAssetId);

		if (signalState.Configuration >= signalAsset.SignalConfigurations.Num() || signalState.Configuration < 0)
		{
			UE_LOG(RoadRunnerImporter, Warning, TEXT("Signal Configuration for %s out of range."), *signalState.Id);
			return signalState;
		}
		const FSignalConfiguration& signalConfiguration = signalAsset.SignalConfigurations[signalState.Configuration];

		// Loop over light bulb states and set up the light instance states
		for (const auto& lightBulbState : signalConfiguration.LightBulbStates)
		{
			if (uuidToComponentMap.count(signalState.Id) == 0)
			{
				UE_LOG(RoadRunnerImporter, Warning, TEXT("Signal %s not found inside this blueprint."), *signalState.Id);
				continue;
			}
			FLightInstanceState lightInstanceState;
			lightInstanceState.ComponentName = FindByNamePrefix(uuidToComponentMap.at(signalState.Id), lightBulbState.Name);
			lightInstanceState.State = lightBulbState.State;
			signalState.LightInstanceStates.Add(lightInstanceState);
		}

		return signalState;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "Interval" element. Contains the "Time" of its duration, and multiple
	// "Signal" states.
	FLightInterval LoadInterval(FXmlNode* intervalNode, const std::unordered_map<FString, USCS_Node*, FStringHash>& uuidToComponentMap, const std::unordered_map<FString, FSignalAsset, FStringHash>& uuidToSignalAssetMap)
	{
		FLightInterval interval;
		for (const auto& intervalProperty : intervalNode->GetChildrenNodes())
		{
			const FString& tag = intervalProperty->GetTag();
			if (tag.Equals(TEXT("Time"), ESearchCase::CaseSensitive))
			{
				interval.Time = FCString::Atof(*intervalProperty->GetContent());
			}
			else if (tag.Equals(TEXT("Signal"), ESearchCase::CaseSensitive))
			{
				FSignalState signalState = LoadSignalState(intervalProperty, uuidToComponentMap, uuidToSignalAssetMap);
				interval.SignalStates.Add(signalState);
			}
		}
		return interval;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "SignalPhase" element. Contains multiple "Interval" elements.
	FSignalPhase LoadSignalPhase(FXmlNode* signalPhaseNode, const std::unordered_map<FString, USCS_Node*, FStringHash>& uuidToComponentMap, const std::unordered_map<FString, FSignalAsset, FStringHash>& uuidToSignalAssetMap)
	{
		FSignalPhase signalPhase;
		for (const auto& signalPhaseProperty : signalPhaseNode->GetChildrenNodes())
		{
			const FString& tag = signalPhaseProperty->GetTag();
			if (tag.Equals(TEXT("Interval"), ESearchCase::CaseSensitive))
			{
				FLightInterval interval = LoadInterval(signalPhaseProperty, uuidToComponentMap, uuidToSignalAssetMap);
				signalPhase.Intervals.Add(interval);
			}
		}
		return signalPhase;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Parses "Junction" element. Contains its "ID" and multiple "SignalPhase" elements
	FJunction LoadJunction(FXmlNode* junctionNode, const std::unordered_map<FString, USCS_Node*, FStringHash>& uuidToComponentMap, const std::unordered_map<FString, FSignalAsset, FStringHash>& uuidToSignalAssetMap)
	{
		FJunction junction;
		for (const auto& junctionProperty : junctionNode->GetChildrenNodes())
		{
			const FString& tag = junctionProperty->GetTag();
			if (tag.Equals(TEXT("ID"), ESearchCase::CaseSensitive))
			{
				junction.Id = junctionProperty->GetContent();
			}
			else if (tag.Equals(TEXT("SignalPhase"), ESearchCase::CaseSensitive))
			{
				FSignalPhase signalPhase = LoadSignalPhase(junctionProperty, uuidToComponentMap, uuidToSignalAssetMap);
				junction.SignalPhases.Add(signalPhase);
			}

		}
		return junction;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Load the Junctions into the array.
	TArray<FJunction> LoadSignalJunctions(FXmlNode* xmlSignalData, const std::unordered_map<FString, USCS_Node*, FStringHash>& uuidToComponentMap, const std::unordered_map<FString, FSignalAsset, FStringHash>& uuidToSignalAssetMap)
	{
		TArray<FJunction> ret = TArray<FJunction>();

		if (!xmlSignalData)
			return ret;

		// "SignalData" has multiple Junction elements under it
		for (const auto& signalDataProperty : xmlSignalData->GetChildrenNodes())
		{
			const FString& tag = signalDataProperty->GetTag();
			if (tag.Equals(TEXT("Junction"), ESearchCase::CaseSensitive))
			{
				FJunction junction = LoadJunction(signalDataProperty, uuidToComponentMap, uuidToSignalAssetMap);
				ret.Add(junction);
			}
		}

		return ret;
	}
}

////////////////////////////////////////////////////////////////////////////////
// If fbx was imported through our scene importer, re-import the materials using
// the metadata file, then parse the signal metadata and attach traffic
// junctions components to the newly created blueprint.
void FRoadRunnerImporterModule::RoadRunnerPostProcessing(UFactory* inFactory, UObject* inCreateObject)
{
	if (!inCreateObject)
	{
		return;
	}
	if (!inFactory->IsA<URoadRunnerFbxSceneImportFactory>())
	{
		return;
	}
	if (inCreateObject->IsA<UWorld>())
	{
		return;
	}

	
	FString srcPath = FPaths::GetPath(*inFactory->GetCurrentFilename());
	FString packagePath = FPaths::GetPath(inCreateObject->GetPathName()) + "/";

	FString rrMetadataFile = FPaths::ChangeExtension(inFactory->GetCurrentFilename(), RoadRunnerExtension);
	if (!FPaths::FileExists(rrMetadataFile))
	{
		return;
	}

	FXmlFile* rrXml = new FXmlFile(rrMetadataFile);
	if (!rrXml->IsValid())
	{
		UE_LOG(RoadRunnerImporter, Error, TEXT("Metadata XML is invalid in: %s"), *(rrMetadataFile));
		return;
	}
	FXmlNode* xmlRoot = rrXml->GetRootNode();

	CurrentMetadataVersion = FCString::Atoi(*(xmlRoot->GetAttribute("Version")));
	if (CurrentMetadataVersion > PluginVersion)
	{
		UE_LOG(RoadRunnerImporter, Warning, TEXT("%s has a version newer than the current plugin. Update the plugin if there are unexpected results."), *(rrMetadataFile));
	}



	FXmlNode * xmlMatList = xmlRoot->FindChildNode("MaterialList");
	if (!xmlMatList)
	{
		UE_LOG(RoadRunnerImporter, Error, TEXT("Material List not found in metadata: %s"), *(rrMetadataFile));
		return;
	}

	TMap<FString, int32> materialToLayerMap;

	const auto& xmlMats = xmlMatList->GetChildrenNodes();

	for (const auto& mat : xmlMats)
	{
		const auto& matProperties = mat->GetChildrenNodes();

		// Fill out material info struct based off xml
		MaterialInfo matInfo;
		for (const auto& matProperty : matProperties)
		{
			const FString& tag = matProperty->GetTag();
			if (tag.Equals(TEXT("Name"), ESearchCase::CaseSensitive))
			{
				matInfo.Name = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("DiffuseMap"), ESearchCase::CaseSensitive))
			{
				matInfo.DiffuseMap = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("NormalMap"), ESearchCase::CaseSensitive))
			{
				matInfo.NormalMap = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("SpecularMap"), ESearchCase::CaseSensitive))
			{
				matInfo.SpecularMap = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("DiffuseColor"), ESearchCase::CaseSensitive))
			{
				matInfo.DiffuseColor = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("TransparentColor"), ESearchCase::CaseSensitive))
			{
				matInfo.TransparencyMap = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("TransparencyFactor"), ESearchCase::CaseSensitive))
			{
				matInfo.TransparencyFactor = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("SpecularColor"), ESearchCase::CaseSensitive))
			{
				matInfo.SpecularColor = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("SpecularFactor"), ESearchCase::CaseSensitive))
			{
				matInfo.SpecularFactor = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("Roughness"), ESearchCase::CaseSensitive))
			{
				matInfo.Roughness = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("Emission"), ESearchCase::CaseSensitive))
			{
				matInfo.Emission = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("TextureScaleU"), ESearchCase::CaseSensitive))
			{
				matInfo.TextureScaleU = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("TextureScaleV"), ESearchCase::CaseSensitive))
			{
				matInfo.TextureScaleV = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("Roughness"), ESearchCase::CaseSensitive))
			{
				matInfo.Roughness = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("TwoSided"), ESearchCase::CaseSensitive))
			{
				matInfo.TwoSided = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("DrawQueue"), ESearchCase::CaseSensitive))
			{
				matInfo.DrawQueue = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("ShadowCaster"), ESearchCase::CaseSensitive))
			{
				matInfo.ShadowCaster = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("IsDecal"), ESearchCase::CaseSensitive))
			{
				matInfo.IsDecal = matProperty->GetContent();
			}
			else if (tag.Equals(TEXT("AmbientColor")))
			{
				// Unused
			}
			else
			{
				UE_LOG(RoadRunnerImporter, Warning, TEXT("Unrecognized element '%s' found in material property"), *tag);
			}
		}

		// Validation
		if (matInfo.Name.IsEmpty())
		{
			UE_LOG(RoadRunnerImporter, Warning, TEXT("Material is missing a name"));
			continue;
		}

		// Follow Unreal's naming scheme
		matInfo.Name = UTF8_TO_TCHAR(MakeName(TCHAR_TO_ANSI(*matInfo.Name)));
		matInfo.Name = ObjectTools::SanitizeObjectName(matInfo.Name);

		CreateUnrealMaterial(srcPath, *packagePath, matInfo, materialToLayerMap);
	}

	// Import as one Blueprint Asset
	UBlueprint* blueprint = Cast<UBlueprint>(inCreateObject);
	if (!blueprint)
	{
		return;
	}
	blueprint->AddToRoot();

	// Set sort priority in blueprint
	for (const auto& uscsnode : blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!uscsnode->ComponentTemplate->IsA<UStaticMeshComponent>())
		{
			continue;
		}
		UStaticMeshComponent* staticMeshComponent = Cast<UStaticMeshComponent>(uscsnode->ComponentTemplate);
		auto slotNames = staticMeshComponent->GetMaterialSlotNames();
		// check mesh has materials
		if (slotNames.Num() <= 0)
		{
			continue;
		}

		// just get the first material to find the layer
		FName matName = slotNames[0];
		FString materialFullName = matName.ToString();
		// Follow Unreal's naming scheme
		materialFullName = UTF8_TO_TCHAR(FRoadRunnerImporterModule::MakeName(TCHAR_TO_ANSI(*materialFullName)));
		materialFullName = ObjectTools::SanitizeObjectName(materialFullName);

		int32* drawQueue = materialToLayerMap.Find(materialFullName);
		if (drawQueue != nullptr)
		{
			staticMeshComponent->SetTranslucentSortPriority(*drawQueue);
		}
	}


	FXmlNode * xmlSignalData = xmlRoot->FindChildNode("SignalData");
	if (!xmlSignalData)
	{
		UE_LOG(RoadRunnerImporter, Error, TEXT("Signal Data not found in metadata: %s"), *(rrMetadataFile));
		return;
	}

	std::unordered_map<FString, USCS_Node*, FStringHash> uuidToComponentMap;
	const FRegexPattern uuidPattern(TEXT("^[{(]?[0-9A-Fa-f]{8}[-]?([0-9A-Fa-f]{4}[-]?){3}[0-9A-Fa-f]{12}[)}]?"));

	// Create uuid to component map
	for (const auto& uscsnode : blueprint->SimpleConstructionScript->GetAllNodes())
	{
		FString nodeName = uscsnode->GetVariableName().ToString();

		FRegexMatcher matcher(uuidPattern, nodeName);

		// Need to check name for uuid, then add uscs_node to a map
		if (matcher.FindNext())
		{
			// Only check first match
			FString match = matcher.GetCaptureGroup(0);

			if (uscsnode->GetChildNodes().Num() != 0)
			{
				uuidToComponentMap[match] = (uscsnode->GetChildNodes())[0];
			}
		}
	}

	std::unordered_map<FString, FSignalAsset, FStringHash> uuidToSignalAssetMap;

	// Create map of signal assets from metadata
	LoadSignalAssets(xmlSignalData, uuidToSignalAssetMap);

	// Parse junction data from xml
	TArray<FJunction> junctions = LoadSignalJunctions(xmlSignalData, uuidToComponentMap, uuidToSignalAssetMap);

	// Add to blueprint
	AActor* dummyActor = NewObject<AActor>();
	dummyActor->AddToRoot();

	TArray<UActorComponent*> newComponents;
	for (const auto& junction : junctions)
	{
		URoadRunnerTrafficJunction* component = NewObject<URoadRunnerTrafficJunction>(dummyActor);
		component->AddToRoot();
		component->SetPhases(junction);
		newComponents.Add(component);
	}

	FKismetEditorUtilities::AddComponentsToBlueprint(blueprint, newComponents, false, (USCS_Node*)nullptr, true);
	
	FKismetEditorUtilities::CompileBlueprint(blueprint);
	blueprint->MarkPackageDirty();
	blueprint->PreEditChange(NULL);
	blueprint->PostEditChange();
	blueprint->RemoveFromRoot();
	dummyActor->RemoveFromRoot();
	for (const auto& comp : newComponents)
	{
		comp->RemoveFromRoot();
	}

	// Replace the original actor in the world with the updated blueprint
	AActor* origActor = GEditor->GetSelectedActors()->GetTop<AActor>();
	if (!origActor)
		return;

	UWorld* world = origActor->GetWorld();
	if (!world)
		return;

	// Deselect the original actor and Destroy it
	GEditor->SelectActor(origActor, false, false);
	GEditor->Layers->DisassociateActorFromLayers(origActor);
	world->EditorDestroyActor(origActor, false);

	// Replace the actor
	AActor* newActor = world->SpawnActor(blueprint->GeneratedClass);

	// Update selection to new actor
	GEditor->SelectActor(newActor, /*bSelected=*/ true, /*bNotify=*/ true);
}

////////////////////////////////////////////////////////////////////////////////

void FRoadRunnerImporterModule::StartupModule()
{
	FEditorDelegates::OnAssetPostImport.AddStatic(&FRoadRunnerImporterModule::RoadRunnerPostProcessing);
}

////////////////////////////////////////////////////////////////////////////////

void FRoadRunnerImporterModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

////////////////////////////////////////////////////////////////////////////////
// Copied directly from FbxMainImport.cpp:1210 on version 4.20
// Replaces invalid characters and cuts off colons
ANSICHAR* FRoadRunnerImporterModule::MakeName(const ANSICHAR* Name)
{
	const int SpecialChars[] = { '.', ',', '/', '`', '%' };

	const int len = FCStringAnsi::Strlen(Name);
	ANSICHAR* TmpName = new ANSICHAR[len + 1];

	FCStringAnsi::Strcpy(TmpName, len + 1, Name);

	for (int32 i = 0; i < ARRAY_COUNT(SpecialChars); i++)
	{
		ANSICHAR* CharPtr = TmpName;
		while ((CharPtr = FCStringAnsi::Strchr(CharPtr, SpecialChars[i])) != NULL)
		{
			CharPtr[0] = '_';
		}
	}

	// Remove namespaces
	ANSICHAR* NewName;
	NewName = FCStringAnsi::Strchr(TmpName, ':');

	// there may be multiple namespace, so find the last ':'
	while (NewName && FCStringAnsi::Strchr(NewName + 1, ':'))
	{
		NewName = FCStringAnsi::Strchr(NewName + 1, ':');
	}

	if (NewName)
	{
		return NewName + 1;
	}

	return TmpName;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRoadRunnerImporterModule, RoadRunnerImporter)
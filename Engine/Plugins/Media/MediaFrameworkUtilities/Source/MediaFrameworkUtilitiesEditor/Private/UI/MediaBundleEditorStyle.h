// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"

class FSlateStyleSet;
class ISlateStyle;

//////////////////////////////////////////////////////////////////////////
// FMediaBundleEditorStyle

class FMediaBundleEditorStyle
{
public:
	static void Register();
	static void Unregister();

	static FName GetStyleSetName();

	static const ISlateStyle& Get();
};
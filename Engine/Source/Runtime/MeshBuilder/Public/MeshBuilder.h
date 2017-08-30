// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInterface.h"

/**
 * Abstract class which is the base class of all builder.
 * All share code to build some render data should be found inside this class
 */
class MESHBUILDER_API FMeshBuilder
{
public:
	FMeshBuilder();
	
	/**
	 * Build function should be override and is the starting point for all mesh builders
	 */
	virtual bool Build(class UStaticMesh* StaticMesh) =0;

private:

};


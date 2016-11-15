// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.


#include "CurveTableEditorPrivatePCH.h"
#include "CurveTableEditorCommands.h"

#define LOCTEXT_NAMESPACE "CurveTableEditorCommands"

void FCurveTableEditorCommands::RegisterCommands()
{
	UI_COMMAND(CurveViewToggle, "Curve View", "Changes the view of the curve table from grid to curve view.", EUserInterfaceActionType::ToggleButton, FInputChord());
}

#undef LOCTEXT_NAMESPACE
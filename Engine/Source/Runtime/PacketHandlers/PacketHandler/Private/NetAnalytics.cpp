// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

// Includes
#include "NetAnalytics.h"
#include "PacketHandler.h"
#include "NetAnalyticsAggregatorConfig.h"


// Globals

TAtomic<uint8> GNetAnalyticsCounter(0);


/**
 * FNetAnalytics
 */

void FNetAnalyticsData::InternalSendAnalytics()
{
	SendAnalytics();
}

#if NET_ANALYTICS_MULTITHREADING
/**
 * FThreadedNetAnalyticsData
 */

FThreadedNetAnalyticsData::FThreadedNetAnalyticsData()
	: FNetAnalyticsData()
	, bReadyToSend(false)
{
}

void FThreadedNetAnalyticsData::InternalSendAnalytics()
{
	bReadyToSend = true;
}

void FThreadedNetAnalyticsData::NotifyFinalRelease()
{
	if (bReadyToSend)
	{
		SendAnalytics();
	}
}
#endif


/**
 * FNetAnalyticsAggregator
 */

FNetAnalyticsAggregator::FNetAnalyticsAggregator(TSharedPtr<IAnalyticsProvider> InProvider, FName InNetDriverName)
	: AnalyticsProvider(InProvider)
	, NetDriverName(InNetDriverName)
	, AnalyticsDataMap()
	, AnalyticsDataTypeMap()
	, AnalyticsDataConfigMap()
	, bSentAnalytics(false)
{
}

void FNetAnalyticsAggregator::Init()
{
	GNetAnalyticsCounter++;

	InitConfig();
}

void FNetAnalyticsAggregator::InitConfig()
{
	UNetAnalyticsAggregatorConfig* CurConfig = NewObject<UNetAnalyticsAggregatorConfig>((UObject*)GetTransientPackage(), NetDriverName);

	AnalyticsDataConfigMap.Empty();

	// If the config is hotfixed, make sure no data holders are currently active, as they can't be selectively hotfixed if loaded
	ensure(AnalyticsDataMap.Num() == 0);

	if (CurConfig != nullptr)
	{
		for (const FNetAnalyticsDataConfig& CurEntry : CurConfig->NetAnalyticsData)
		{
			AnalyticsDataConfigMap.Add(CurEntry.DataName, CurEntry.bEnabled);

			UE_LOG(PacketHandlerLog, Log, TEXT("Adding NetAnalyticsData: %s, bEnabled: %i"), *CurEntry.DataName.ToString(),
					CurEntry.bEnabled);
		}
	}
}

TNetAnalyticsDataPtr<> FNetAnalyticsAggregator::RegisterAnalyticsData_Internal(TNetAnalyticsDataRef<> InData, const FName& InDataName,
																					FString InTypeName)
{
	TNetAnalyticsDataPtr<> Result;
	bool* bEnabled = AnalyticsDataConfigMap.Find(InDataName);

	if (bEnabled != nullptr && *bEnabled)
	{
		FString* TypeVal = AnalyticsDataTypeMap.Find(InDataName);

		if (TypeVal == nullptr)
		{
			AnalyticsDataTypeMap.Add(InDataName, InTypeName);
		}
		else
		{
			check(*TypeVal == InTypeName);
		}

		TNetAnalyticsDataRef<>* Found = AnalyticsDataMap.Find(InDataName);

		Result = Found != nullptr ? *Found : AnalyticsDataMap.Add(InDataName, InData);

		InData->Aggregator = this;
	}
	else if (bEnabled == nullptr)
	{
		UE_LOG(PacketHandlerLog, Error, TEXT("NetAnalyticsData type '%s' must be added to NetAnalyticsAggregatorConfig, for NetDriverName: %s."),
				*InDataName.ToString(), *NetDriverName.ToString());
	}

	return Result;
}

void FNetAnalyticsAggregator::SendAnalytics()
{
	if (!bSentAnalytics)
	{
		for (TMap<FName, TNetAnalyticsDataRef<>>::TConstIterator It(AnalyticsDataMap); It; ++It)
		{
			It.Value()->InternalSendAnalytics();
		}

		bSentAnalytics = true;
	}
}

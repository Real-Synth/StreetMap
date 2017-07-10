#include "StreetMapImporting.h"
#include "StreetMapComponent.h"
#include "Elevation.h"

#include "DesktopPlatformModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "HttpManager.h"
#include "Misc/ScopedSlowTask.h"
#include "Interfaces/IImageWrapperModule.h"
#include "SNotificationList.h"
#include "NotificationManager.h"
#include "ScopedTransaction.h"
#include "SpatialReferenceSystem.h"
#include "TiledMap.h"
#include "LandscapeInfo.h"
#include "Polygon2DView.h"

#define LOCTEXT_NAMESPACE "StreetMapImporting"

static const int LanczosFilterSize = 3;

static void ShowErrorMessage(const FText& MessageText)
{
	FNotificationInfo Info(MessageText);
	Info.ExpireDuration = 8.0f;
	Info.bUseLargeFont = false;
	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Fail);
		Notification->ExpireAndFadeout();
	}
}

static const FString& GetElevationCacheDir()
{
	static FString ElevationCacheDir;

	if (!ElevationCacheDir.Len())
	{
		const FString UserTempDir = FPaths::ConvertRelativePathToFull(FDesktopPlatformModule::Get()->GetUserTempPath());
		ElevationCacheDir = FString::Printf(TEXT("%s%s"), *UserTempDir, TEXT("ElevationCache/"));
	}
	return ElevationCacheDir;
}

static FString GetCachedFilePath(uint32 X, uint32 Y, uint32 Z)
{
	FString FilePath = GetElevationCacheDir();
	FilePath.Append("elevation_");
	FilePath.AppendInt(Z);
	FilePath.Append("_");
	FilePath.AppendInt(X);
	FilePath.Append("_");
	FilePath.AppendInt(Y);
	FilePath.Append(".png");
	return FilePath;
}


/** Represents a file from a Tiled Elevation Map Service. 
 *	Its functionality includes elevation data decoding and caching to disk.
 */
class FCachedElevationFile
{
private:
	const int32 MaxNumPendingDownloads = 10;
	static int32 NumPendingDownloads;

	const FTiledMap& TiledMap;

	bool WasInitialized;
	bool WasDownloadSuccessful;
	bool Failed;

	FDateTime StartTime;
	FTimespan TimeSpan;

	TSharedPtr<IHttpRequest> HttpRequest;

	bool UnpackElevation(const TArray<uint8>& RawData)
	{
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

		IImageWrapperPtr PngImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (PngImageWrapper.IsValid() && PngImageWrapper->SetCompressed(RawData.GetData(), RawData.Num()))
		{
			int32 BitDepth = PngImageWrapper->GetBitDepth();
			const ERGBFormat::Type Format = PngImageWrapper->GetFormat();
			const int32 Width = PngImageWrapper->GetWidth();
			const int32 Height = PngImageWrapper->GetHeight();

			if (Width != TiledMap.TileWidth || Height != TiledMap.TileHeight)
			{
				GWarn->Logf(ELogVerbosity::Error, TEXT("PNG file has wrong dimensions. Expected %dx%d"), TiledMap.TileWidth, TiledMap.TileHeight);
				return false;
			}

			if ((Format != ERGBFormat::RGBA) || BitDepth != 8)
			{
				GWarn->Logf(ELogVerbosity::Error, TEXT("PNG file contains elevation data in an unsupported format."));
				return false;
			}

			const TArray<uint8>* RawPNG = nullptr;
			if (PngImageWrapper->GetRaw(Format, BitDepth, RawPNG))
			{
				const uint8* Data = RawPNG->GetData();
				Elevation.SetNumUninitialized(Width * Height);
				float* ElevationData = Elevation.GetData();
				const float* ElevationDataEnd = ElevationData + (Width * Height);

				while (ElevationData < ElevationDataEnd)
				{
					float ElevationValue = (Data[0] * 256.0f + Data[1] + Data[2] / 256.0f);

					const bool IsValid = (ElevationValue > 0.0f) && (ElevationValue < 41768.0f); // smaller than Mount Everest?
					if (IsValid)
					{
						ElevationValue -= 32768.0f;
						ElevationMin = FMath::Min(ElevationMin, ElevationValue);
						ElevationMax = FMath::Max(ElevationMax, ElevationValue);
					}

					*ElevationData = ElevationValue;

					ElevationData++;
					Data += 4;
				}
			}

			return true;
		}

		return false;
	}

	void OnDownloadSucceeded(FHttpResponsePtr Response)
	{
		NumPendingDownloads--;

		// unpack data
		if (Response.IsValid())
		{
			const TArray<uint8>& Content = Response->GetContent();
			if (!UnpackElevation(Content))
			{
				Failed = true;
				return;
			}

			// write data to cache
			FFileHelper::SaveArrayToFile(Content, *GetCachedFilePath(X, Y, Z));
		}

		WasDownloadSuccessful = true;
	}

	void DownloadFile()
	{
		FString URL = FString::Printf(*TiledMap.URLTemplate, Z, X, Y);

		HttpRequest = FHttpModule::Get().CreateRequest();
		HttpRequest->SetVerb(TEXT("GET"));

		HttpRequest->SetURL(URL);
		if (HttpRequest->ProcessRequest())
		{
			NumPendingDownloads++;
		}
		else
		{
			Failed = true;
		}
	}

	void Initialize()
	{
		WasInitialized = true;
		StartTime = FDateTime::UtcNow();

		// try to load data from cache first
		{
			TArray<uint8> RawData;
			if (FFileHelper::LoadFileToArray(RawData, *GetCachedFilePath(X, Y, Z), FILEREAD_Silent))
			{
				if (UnpackElevation(RawData))
				{
					WasDownloadSuccessful = true;
					return;
				}
			}
		}
		
		DownloadFile();
	}

protected:
	TArray<float> Elevation;
	uint32 X, Y, Z;

	float ElevationMin;
	float ElevationMax;

	friend class FPaddedElevationTile;
	friend class FElevationModel;

public:

	bool HasFinished() const
	{
		return WasDownloadSuccessful || Failed;
	}

	bool Succeeded() const
	{
		return WasDownloadSuccessful;
	}

	void CancelRequest()
	{
		Failed = true;
		if (HttpRequest.IsValid())
		{
			NumPendingDownloads--;
			HttpRequest->CancelRequest();
		}
	}

	void Tick()
	{
		if (!WasInitialized)
		{
			if (NumPendingDownloads >= MaxNumPendingDownloads)
			{
				return;
			}

			Initialize();
		}

		if (WasDownloadSuccessful || Failed) return;

		if (TimeSpan.GetSeconds() > 10)
		{
			GWarn->Logf(ELogVerbosity::Error, TEXT("Download time-out. Check your internet connection!"));
			NumPendingDownloads--;
			Failed = true;
			HttpRequest->CancelRequest();
			return;
		}
		else
		{
			TimeSpan = FDateTime::UtcNow() - StartTime;
		}

		if (HttpRequest->GetStatus() == EHttpRequestStatus::Failed ||
			HttpRequest->GetStatus() == EHttpRequestStatus::Failed_ConnectionError)
		{
			GWarn->Logf(ELogVerbosity::Error, TEXT("Download connection failure. Check your internet connection!"));
			NumPendingDownloads--;
			Failed = true;
			HttpRequest->CancelRequest();
			return;
		}

		if (HttpRequest->GetStatus() == EHttpRequestStatus::Succeeded)
		{
			OnDownloadSucceeded(HttpRequest->GetResponse());
			return;
		}

		HttpRequest->Tick(0);
	}

	FCachedElevationFile(const FTiledMap& TiledMap, uint32 X, uint32 Y, uint32 Z)
		: TiledMap(TiledMap)
		, WasInitialized(false)
		, WasDownloadSuccessful(false)
		, Failed(false)
		, StartTime(FDateTime::UtcNow())
		, X(X)
		, Y(Y)
		, Z(Z)
		, ElevationMin(TNumericLimits<float>::Max())
		, ElevationMax(TNumericLimits<float>::Lowest())
	{
	}
};


/** A Padded Elevation Tile. Useful for resampling the elevation data. */
class FPaddedElevationTile
{
private:
	void CopyRegion(const TArray<TSharedPtr<FCachedElevationFile>>& CachedElevationFiles,
					 int32 TileX, int32 TileY, uint32 LevelIndex,
					 uint32 SrcX, uint32 SrcY,
					 uint32 DstX, uint32 DstY,
					 uint32 StrideX, uint32 SrideY)
	{
		const FCachedElevationFile* SrcTile = GetTile(CachedElevationFiles, TileX, TileY, LevelIndex);
		
		float* DstTileElevationData = Elevation.GetData();

		if (!SrcTile)
		{
			for (uint32 Row = 0; Row < SrideY; Row++)
			{
				FMemory::Memset(&DstTileElevationData[Height * (DstY + Row) + DstX], 0, StrideX * sizeof(float));
			}
			return;
		}

		const uint32 SrcTileWidth = SrcTile->TiledMap.TileWidth;
		const uint32 SrcTileHeight = SrcTile->TiledMap.TileHeight;
		const float* SrcTileElevationData = SrcTile->Elevation.GetData();

		for (uint32 Row = 0; Row < SrideY; Row++)
		{
			FMemory::Memcpy(&DstTileElevationData[Height * (DstY + Row) + DstX],
							&SrcTileElevationData[SrcTileHeight * (SrcY + Row) + SrcX], StrideX * sizeof(float));
		}
	}

protected:
	TArray<float> Elevation;
	uint32 X, Y, Z;
	uint32 Width, Height;
	uint32 PaddingX, PaddingY;

	friend class FElevationModel;

public:

	FPaddedElevationTile(const TArray<TSharedPtr<FCachedElevationFile>>& CachedElevationFiles,
						 const FTiledMap& TiledMap, const FIntPoint& XY, uint32 LevelIndex)
		: X(XY.X)
		, Y(XY.Y)
		, Z(LevelIndex)
	{
		const uint32 TileWidth = TiledMap.TileWidth;
		const uint32 TileHeight = TiledMap.TileHeight;

		PaddingX = LanczosFilterSize - 1;
		PaddingY = LanczosFilterSize - 1;
		Width = TileWidth + PaddingX * 2;
		Height = TileHeight + PaddingY * 2;

		// fill elevation data including padding
		Elevation.SetNumUninitialized(Width * Height);

		// central main part
		CopyRegion(CachedElevationFiles, XY.X + 0, XY.Y + 0, LevelIndex, 0, 0, PaddingX, PaddingY, TileWidth, TileHeight);
		
		// top row padding 
		CopyRegion(CachedElevationFiles, XY.X + 0, XY.Y - 1, LevelIndex, 0, TileHeight - PaddingY, PaddingX, 0, TileWidth, PaddingY);
		// bottom row padding
		CopyRegion(CachedElevationFiles, XY.X + 0, XY.Y + 1, LevelIndex, 0, 0, PaddingX, Height - PaddingY, TileWidth, PaddingY);
		// left column padding
		CopyRegion(CachedElevationFiles, XY.X - 1, XY.Y + 0, LevelIndex, TileWidth - PaddingX, 0, 0, PaddingY, PaddingX, TileHeight);
		// right column padding
		CopyRegion(CachedElevationFiles, XY.X + 1, XY.Y + 0, LevelIndex, 0, 0, Width - PaddingX, PaddingY, PaddingX, TileHeight);
		// top left corner padding
		CopyRegion(CachedElevationFiles, XY.X - 1, XY.Y - 1, LevelIndex, TileWidth - PaddingX, TileHeight - PaddingY, 0, 0, PaddingX, PaddingY);
		// top right corner padding
		CopyRegion(CachedElevationFiles, XY.X + 1, XY.Y - 1, LevelIndex, 0, TileHeight - PaddingY, Width - PaddingX, 0, PaddingX, PaddingY);
		// bottom left corner padding
		CopyRegion(CachedElevationFiles, XY.X - 1, XY.Y + 1, LevelIndex, TileWidth - PaddingX, 0, 0, Height - PaddingY, PaddingX, PaddingY);
		// bottom right corner padding
		CopyRegion(CachedElevationFiles, XY.X + 1, XY.Y + 1, LevelIndex, 0, 0, Width - PaddingX, Height - PaddingY, PaddingX, PaddingY);
	}


private:

	const FCachedElevationFile* GetTile(const TArray<TSharedPtr<FCachedElevationFile>>& CachedElevationFiles,
										int32 TileX, int32 TileY, uint32 LevelIndex)
	{
		for (const TSharedPtr<FCachedElevationFile>& Tile : CachedElevationFiles)
		{
			if (Tile->X == TileX && Tile->Y == TileY && Tile->Z == LevelIndex)
			{
				return Tile.Get();
			}
		}
		return nullptr;
	}
};

int32 FCachedElevationFile::NumPendingDownloads = 0;

static int32 GetNumVerticesForRadius(const FStreetMapLandscapeBuildSettings& BuildSettings, int32& OutSubsectionSizeQuads)
{
	int32 Size = FMath::RoundToInt(BuildSettings.Radius / BuildSettings.QuadSize);
	OutSubsectionSizeQuads = FMath::RoundUpToPowerOfTwo(Size) / 16 - 1;
	Size = FMath::DivideAndRoundUp(Size, OutSubsectionSizeQuads) * OutSubsectionSizeQuads;
	return Size;
}

// @todo: replace these by the real engine values
static const float DefaultLandscapeScaleXY = 128.0f;
static const float DefaultLandscapeScaleZ = 256.0f;
static const float OSMToCentimetersScaleFactor = 100.0f;

/** Loads and transforms elevation data for desired region. */
class FElevationModel
{
public:

	FElevationModel(const FTiledMap& TiledMap)
		: TiledMap(TiledMap)
		, ElevationMin(TNumericLimits<float>::Max())
		, ElevationMax(TNumericLimits<float>::Lowest())
	{
	}

	const FTransform& GetTransform() const
	{
		return Transform;
	}

	bool LoadElevationData(UStreetMapComponent* StreetMapComponent, const FStreetMapLandscapeBuildSettings& BuildSettings, FScopedSlowTask& SlowTask)
	{
		TArray<TSharedPtr<FCachedElevationFile>> FilesToDownload;

		// 1.) collect all elevation tiles needed based on StreetMap location and Landscape size
		{
			const UStreetMap* StreetMap = StreetMapComponent->GetStreetMap();
			const FSpatialReferenceSystem SRS(StreetMap->GetOriginLongitude(), StreetMap->GetOriginLatitude());

			int32 SubsectionSizeQuads;
			const int32 NumVerticesForRadius = GetNumVerticesForRadius(BuildSettings, SubsectionSizeQuads);
			const float FinalRadius = NumVerticesForRadius * BuildSettings.QuadSize;

			const FVector2D SouthWest(-FinalRadius, FinalRadius);
			const FVector2D NorthEast(FinalRadius, -FinalRadius);
			double South, West, North, East;
			if (!SRS.ToEPSG3857(SouthWest, West, South) || !SRS.ToEPSG3857(NorthEast, East, North))
			{
				ShowErrorMessage(LOCTEXT("ElevationBoundsInvalid", "Chosen elevation bounds are invalid. Stay within WebMercator bounds!"));
				return false;
			}

			// download highest resolution available
			const uint32 LevelIndex = TiledMap.NumLevels - 1;
			const int32  NumTiles = 1 << LevelIndex;
			const FIntPoint SouthWestTileXY = TiledMap.GetTileXY(West, South, LevelIndex);
			const FIntPoint NorthEastTileXY = TiledMap.GetTileXY(East, North, LevelIndex);

			// since we may not know the direction of tile order the source uses we need to order them
			// additionally download padded frame around needed area to cover enoúgh for the lanczos filter
			const int32 MinX = FMath::Max(FMath::Min(SouthWestTileXY.X, NorthEastTileXY.X) - 1, 0);
			const int32 MinY = FMath::Max(FMath::Min(SouthWestTileXY.Y, NorthEastTileXY.Y) - 1, 0);
			const int32 MaxX = FMath::Min(FMath::Max(SouthWestTileXY.X, NorthEastTileXY.X) + 1, NumTiles - 1);
			const int32 MaxY = FMath::Min(FMath::Max(SouthWestTileXY.Y, NorthEastTileXY.Y) + 1, NumTiles - 1);

			for (int32 Y = MinY; Y <= MaxY; Y++)
			{
				for (int32 X = MinX; X <= MaxX; X++)
				{
					FilesToDownload.Add(MakeShared<FCachedElevationFile>(TiledMap, X, Y, LevelIndex));
				}
			}
		}

		// 2.) download the data from web service or disk if already cached
		const int32 NumFilesToDownload = FilesToDownload.Num();
		while (FilesToDownload.Num())
		{
			FHttpModule::Get().GetHttpManager().Tick(0);

			if (GWarn->ReceivedUserCancel())
			{
				break;
			}

			float Progress = 0.0f;
			for (TSharedPtr<FCachedElevationFile> FileToDownload : FilesToDownload)
			{
				FileToDownload->Tick();

				if (FileToDownload->HasFinished())
				{
					FilesToDownload.Remove(FileToDownload);
					Progress = 1.0f / (float)NumFilesToDownload;

					if (FileToDownload->Succeeded())
					{
						FilesDownloaded.Add(FileToDownload);

						ElevationMin = FMath::Min(ElevationMin, FileToDownload->ElevationMin);
						ElevationMax = FMath::Max(ElevationMax, FileToDownload->ElevationMax);
					}
					else
					{
						// We failed to download one file so cancel the rest because we cannot proceed without it.
						for (TSharedPtr<FCachedElevationFile> FileToCancel : FilesToDownload)
						{
							FileToCancel->CancelRequest();
						}
						FilesToDownload.Empty();
					}
					break;
				}
			}

			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("NumFilesDownloaded"), FText::AsNumber(FilesDownloaded.Num()));
			Arguments.Add(TEXT("NumFilesToDownload"), FText::AsNumber(NumFilesToDownload));
			SlowTask.EnterProgressFrame(Progress, FText::Format(LOCTEXT("DownloadingElevationModel", "Downloading Elevation Model ({NumFilesDownloaded} of {NumFilesToDownload})"), Arguments));

			if (Progress == 0.0f)
			{
				FPlatformProcess::Sleep(0.1f);
			}
		}

		if (FilesDownloaded.Num() < NumFilesToDownload)
		{
			ShowErrorMessage(LOCTEXT("DownloadElevationFailed", "Could not download all necessary elevation model files. See Log for details!"));
			return false;
		}

		return true;
	}

	bool ReprojectData(UStreetMapComponent* StreetMapComponent, const FStreetMapLandscapeBuildSettings& BuildSettings, FScopedSlowTask& SlowTask, TArray<uint16>& OutElevationData)
	{
		const FText ProgressText = LOCTEXT("ReprojectingElevationModel", "Reprojecting Elevation Model");
		const UStreetMap* StreetMap = StreetMapComponent->GetStreetMap();
		const FSpatialReferenceSystem SRS(StreetMap->GetOriginLongitude(), StreetMap->GetOriginLatitude());

		int32 SubsectionSizeQuads;

		const uint32 LevelIndex = TiledMap.NumLevels - 1;
		const int32 NumVerticesForRadius = GetNumVerticesForRadius(BuildSettings, SubsectionSizeQuads);
		const int32 Size = NumVerticesForRadius * 2;
		const float ElevationRange = ElevationMax - ElevationMin;
		const float ElevationScale = 65535.0f / ElevationRange;

		const float ProgressPerRow = 1.0f / Size;

		// sample elevation value for each height map vertex
		OutElevationData.SetNumUninitialized(Size * Size);
		uint16* Elevation = OutElevationData.GetData();
		for (int32 Y = -NumVerticesForRadius; Y < NumVerticesForRadius; Y++)
		{
			for (int32 X = -NumVerticesForRadius; X < NumVerticesForRadius; X++)
			{
				uint16 QuantizedElevation = 32768;

				double WebMercatorX, WebMercatorY;
				FVector2D VertexLocation(X * BuildSettings.QuadSize, Y * BuildSettings.QuadSize);
				if (SRS.ToEPSG3857(VertexLocation, WebMercatorX, WebMercatorY))
				{
					FVector2D PixelXY;
					const FIntPoint TileXY = TiledMap.GetTileXY(WebMercatorX, WebMercatorY, LevelIndex, PixelXY);
					const FPaddedElevationTile* Tile = GetPaddedTile(TileXY, LevelIndex);

					PixelXY.X += Tile->PaddingX;
					PixelXY.Y += Tile->PaddingY;

					const float ElevationValue = SampleElevationLanczos(Tile, PixelXY);
					const float ScaledElevationValue = (ElevationValue - ElevationMin) * ElevationScale;

					QuantizedElevation = (uint16)FMath::RoundToInt(ScaledElevationValue);
				}

				*Elevation = QuantizedElevation;
				Elevation++;
			}

			SlowTask.EnterProgressFrame(ProgressPerRow, ProgressText);

			if (GWarn->ReceivedUserCancel())
			{
				return false;
			}
		}

		// compute exact scale of landscape
		// Landscape docs say: At Z Scale = 100 landscape has an height range limit of -256m:256. 
		const float LandscapeInternalScaleZ = 512.0f / 100.0f;
		const float ScaleXY = OSMToCentimetersScaleFactor * BuildSettings.QuadSize / DefaultLandscapeScaleXY;
		const float ScaleZ = ElevationRange / DefaultLandscapeScaleZ / LandscapeInternalScaleZ;
		const FVector Scale3D(ScaleXY, ScaleXY, ScaleZ);
		Transform.SetScale3D(Scale3D);

		return true;
	}

private:
	const FTiledMap TiledMap;
	TArray<TSharedPtr<FCachedElevationFile>> FilesDownloaded;
	TArray<TSharedPtr<FPaddedElevationTile>> CachedPaddedTiles;
	FTransform Transform;

	float ElevationMin;
	float ElevationMax;

	const FPaddedElevationTile* GetPaddedTile(const FIntPoint& XY, uint32 LevelIndex)
	{
		for (const TSharedPtr<FPaddedElevationTile>& Tile : CachedPaddedTiles)
		{
			if (Tile->X == XY.X && Tile->Y == XY.Y && Tile->Z == LevelIndex)
			{
				return Tile.Get();
			}
		}
		// create the missing tile
		TSharedPtr<FPaddedElevationTile> PaddedTile = MakeShared<FPaddedElevationTile>(FilesDownloaded, TiledMap, XY, LevelIndex);
		CachedPaddedTiles.Add(PaddedTile);
		return PaddedTile.Get();
	}

	template<int FilterSize>
	static float EvalLanczos(float X)
	{
		const float FilterSizeFloat = FilterSize;

		// we don't need this since we never sample outside of the window
		//if (x <= -FilterSizeFloat || x >= FilterSizeFloat)
		//{
		//	return 0.0f;  // Outside of the window
		//}

		if (X > -0.0001 &&
			X <  0.0001)
		{
			return 1.0f;  // Special case (the discontinuity at the origin)
		}

		float XPI = X * PI;
		float XUnit = XPI / FilterSizeFloat;
		float XPISquared = XPI * XPI;

		float SincX = FMath::Sin(XPI);
		float SincXUnit = FMath::Sin(XUnit);

		return FilterSizeFloat * SincX * SincXUnit / XPISquared;
	}

	static float SampleElevationLanczos(const FPaddedElevationTile* Tile, const FVector2D& PixelXY)
	{
		const float* ElevationData = Tile->Elevation.GetData();
		const uint32 DataWidth = Tile->Width;

		// 5x5 footprint with corners dropped (because outside of lanczos kernel) results in 13 taps
		static_assert(LanczosFilterSize == 3, "Sample taps are optimized for filter size 3");
		const int NumTaps = 13;
		const FIntPoint Taps[NumTaps] = {			FIntPoint(0,-2),
								  FIntPoint(-1,-1), FIntPoint(0,-1), FIntPoint(1,-1),
				FIntPoint(-2, 0), FIntPoint(-1, 0), FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(2, 0),
								  FIntPoint(-1, 1), FIntPoint(0, 1), FIntPoint(1, 1),
													FIntPoint(0, 2)
		};
		const FIntPoint* TapsEnd = &Taps[NumTaps];

		int32 ElevationX = (int32)PixelXY.X;
		int32 ElevationY = (int32)PixelXY.Y;
		const float X = PixelXY.X - ElevationX;
		const float Y = PixelXY.Y - ElevationY;

		float ElevationValue = 0.0f;
		float LanczosWeightSum = 0.0f;

		const FIntPoint* Tap = Taps;
		do
		{
			const float TapX = X - Tap->X;
			const float TapY = Y - Tap->Y;
			const float Distance = FMath::Sqrt(TapX * TapX + TapY * TapY);

			const float LanczosWeight = EvalLanczos<LanczosFilterSize>(Distance);
			const float ElevationTapValue = ElevationData[DataWidth * (ElevationY + Tap->Y) + ElevationX + Tap->X] * LanczosWeight;

			ElevationValue += ElevationTapValue;
			LanczosWeightSum += LanczosWeight;

			Tap++;
		} 
		while (Tap < TapsEnd);

		return ElevationValue / LanczosWeightSum;
	}

	static float SampleElevationNearest(const float* ElevationData, const uint32 DataWidth, const uint32 DataHeight, const FVector2D& PixelXY)
	{
		int32 ElevationX = (int32)PixelXY.X;
		int32 ElevationY = (int32)PixelXY.Y;
		float ElevationValue = ElevationData[DataWidth * ElevationY + ElevationX];
		return ElevationValue;
	}
};


bool operator==(const FWayMatch& LHS, const FWayMatch& RHS)
{
	return LHS.Type == RHS.Type && LHS.Category == RHS.Category;
}

static void GetPolygonWaysForLayer(const FName& LayerName, const UStreetMap* StreetMap, const FStreetMapLandscapeBuildSettings& BuildSettings, TArray<const FStreetMapMiscWay*>& OutPolygons)
{
	const FLayerWayMapping* Mapping = BuildSettings.LayerWayMapping.FindByPredicate(
		[LayerName](const FLayerWayMapping& LayerWayMapping)
	{
		return LayerWayMapping.LayerName == LayerName;
	});

	if (!Mapping)
	{
		return;
	}

	const TArray<FStreetMapMiscWay>& MiscWays = StreetMap->GetMiscWays();
	for (const FStreetMapMiscWay& MiscWay : MiscWays)
	{
		if (!MiscWay.bIsClosed) continue;

		const FWayMatch WayMatch(MiscWay.Type, MiscWay.Category);
		if (Mapping->Matches.Contains(WayMatch))
		{
			OutPolygons.Add(&MiscWay);
		}
	}
}

static ALandscape* CreateLandscape(
	UStreetMapComponent* StreetMapComponent, 
	const FStreetMapLandscapeBuildSettings& BuildSettings, 
	const FTransform& Transform, 
	const TArray<uint16>& ElevationData, 
	FScopedSlowTask& SlowTask)
{
	FScopedTransaction Transaction(LOCTEXT("Undo", "Creating New Landscape"));

	UWorld* World = StreetMapComponent->GetOwner()->GetWorld();
	UStreetMap* StreetMap = StreetMapComponent->GetStreetMap();

	int32 SubsectionSizeQuads;
	const int32 NumVerticesForRadius = GetNumVerticesForRadius(BuildSettings, SubsectionSizeQuads);
	const int32 Size = NumVerticesForRadius * 2;
	const FTransform DefaultLandscapeVertexToWorld(FQuat::Identity, FVector::ZeroVector, FVector(DefaultLandscapeScaleXY, DefaultLandscapeScaleXY, DefaultLandscapeScaleZ));
	const FTransform TransformWorld = Transform * DefaultLandscapeVertexToWorld;
	const FTransform TransformLocal = TransformWorld.Inverse();

	// create import layers
	TArray<FLandscapeImportLayerInfo> ImportLayers;
	{
		ImportLayers.Reserve(BuildSettings.Layers.Num());
		const float FillBlendWeightProgress = 1.0f / (BuildSettings.Layers.Num() - 1);
		const FText ProgressText = LOCTEXT("FillingBlendweights", "Rasterizing Blend Weights");

		// Fill in LayerInfos array, allocate blendweight data and fill it according to the land use
		for (const FLandscapeImportLayerInfo& UIImportLayer : BuildSettings.Layers)
		{
			FLandscapeImportLayerInfo ImportLayer = FLandscapeImportLayerInfo(UIImportLayer.LayerName);
			ImportLayer.LayerInfo = UIImportLayer.LayerInfo;
			ImportLayer.SourceFilePath = "";
			ImportLayer.LayerData = TArray<uint8>();

			ImportLayer.LayerData.SetNumUninitialized(Size * Size);
			uint8* WeightData = ImportLayer.LayerData.GetData();

			if (ImportLayers.Num() == 0)
			{
				// Set the first weight-blended layer to 100%
				FMemory::Memset(WeightData, 255, Size * Size);
			}
			else
			{
				// Fill the blend weights based on land use for the other layers
				TArray<const FStreetMapMiscWay*> Polygons;
				GetPolygonWaysForLayer(UIImportLayer.LayerName, StreetMap, BuildSettings, Polygons);

				FMemory::Memset(WeightData, 0, Size * Size);
				if(Polygons.Num() > 0)
				{
					const float FillBlendWeightProgressPerPolygon = FillBlendWeightProgress / Polygons.Num();
					for(const FStreetMapMiscWay* Polygon : Polygons)
					{
						const float BlendGauge = BuildSettings.BlendGauge * OSMToCentimetersScaleFactor;
						const float HalfBlendGauge = BlendGauge * 0.5f;
						const float HalfBlendGaugeSqr = HalfBlendGauge * HalfBlendGauge;

						// Transform polygon AABB into blendweight/vertex space
						FVector Min = TransformLocal.TransformPosition(FVector(Polygon->BoundsMin, 0.0f));
						FVector Max = TransformLocal.TransformPosition(FVector(Polygon->BoundsMax, 0.0f));

						// extend AABB by width of BlendGauge
						Min.X -= HalfBlendGauge;
						Min.Y -= HalfBlendGauge;
						Max.X += HalfBlendGauge;
						Max.Y += HalfBlendGauge;

						// Ensure we do not paint over the limits of the available blendweight area
						const int32 MinX = FMath::Max(-NumVerticesForRadius, FMath::FloorToInt(Min.X));
						const int32 MinY = FMath::Max(-NumVerticesForRadius, FMath::FloorToInt(Min.Y));
						const int32 MaxX = FMath::Min( NumVerticesForRadius - 1, FMath::CeilToInt(Max.X));
						const int32 MaxY = FMath::Min( NumVerticesForRadius - 1, FMath::CeilToInt(Max.Y));

						const FPolygon2DView Polygon2DView(Polygon->Points);
						for (int32 Y = MinY; Y <= MaxY; Y++)
						{
							for (int32 X = MinX; X <= MaxX; X++)
							{
								FVector2D VertexLocation(X * BuildSettings.QuadSize * OSMToCentimetersScaleFactor, 
														 Y * BuildSettings.QuadSize * OSMToCentimetersScaleFactor);

								bool IsInside;
								float SquareDistance = Polygon2DView.ComputeSquareDistance(VertexLocation, IsInside);
								if (IsInside || SquareDistance < HalfBlendGaugeSqr)
								{
									// use distance to polygon to enable smooth blend weights
									const float Lerp = (HalfBlendGauge > SMALL_NUMBER) ? (FMath::Sqrt(SquareDistance) / HalfBlendGauge) * (IsInside ? 0.5f : -0.5f) + 0.5f : 1.0f;
									
									const int32 PixelIndex = (Y + NumVerticesForRadius) * Size + X + NumVerticesForRadius;
									WeightData[PixelIndex] = FMath::Min(255, FMath::RoundToInt(255.0f * Lerp));

									// ramp down the blendweight of this pixel for all other layers
									const float AvailableBlendWeight = (255.0f - WeightData[PixelIndex]) / 255.0f;
									for (auto& PreviousImportLayer : ImportLayers)
									{
										PreviousImportLayer.LayerData[PixelIndex] = FMath::RoundToInt(AvailableBlendWeight * PreviousImportLayer.LayerData[PixelIndex]);
									}
								}
							}
						}

						SlowTask.EnterProgressFrame(FillBlendWeightProgressPerPolygon, ProgressText);

						if (GWarn->ReceivedUserCancel())
						{
							return nullptr;
						}
					}
				}
				else
				{
					*WeightData = 1; // Ensure at least one pixel has a value to keep this layer in editor settings
					SlowTask.EnterProgressFrame(FillBlendWeightProgress, ProgressText);
				}
			}

			ImportLayers.Add(MoveTemp(ImportLayer));
		}
	}

	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("GeneratingLandscapeMesh", "Generating Landscape Mesh"));
	ALandscape* Landscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(), Transform);
	Landscape->LandscapeMaterial = BuildSettings.Material;
	Landscape->Import(FGuid::NewGuid(), 
		-NumVerticesForRadius, -NumVerticesForRadius,
		NumVerticesForRadius - 1, NumVerticesForRadius - 1,
		1, SubsectionSizeQuads, ElevationData.GetData(), nullptr,
		ImportLayers, ELandscapeImportAlphamapType::Additive);

	// automatically calculate a lighting LOD that won't crash lightmass (hopefully)
	//  < 2048x2048 -> LOD0
	// >= 2048x2048 -> LOD1
	// >= 4096x4096 -> LOD2
	// >= 8192x8192 -> LOD3
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((Size * Size) / (2048 * 2048) + 1), (uint32)2);

	// create Landscape info
	{
		ULandscapeInfo* LandscapeInfo = Landscape->CreateLandscapeInfo();
		LandscapeInfo->UpdateLayerInfoMap(Landscape);

		for (int32 i = 0; i < BuildSettings.Layers.Num(); i++)
		{
			if (BuildSettings.Layers[i].LayerInfo != nullptr)
			{
				Landscape->EditorLayerSettings.Add(FLandscapeEditorLayerSettings(BuildSettings.Layers[i].LayerInfo));

				int32 LayerInfoIndex = LandscapeInfo->GetLayerInfoIndex(BuildSettings.Layers[i].LayerName);
				if (ensure(LayerInfoIndex != INDEX_NONE))
				{
					FLandscapeInfoLayerSettings& LayerSettings = LandscapeInfo->Layers[LayerInfoIndex];
					LayerSettings.LayerInfoObj = BuildSettings.Layers[i].LayerInfo;
				}
			}
		}
	}

	return Landscape;
}


ALandscape* BuildLandscape(UStreetMapComponent* StreetMapComponent, const FStreetMapLandscapeBuildSettings& BuildSettings)
{
	FScopedSlowTask SlowTask(4.0f, LOCTEXT("GeneratingLandscape", "Generating Landscape"));
	SlowTask.MakeDialog(true);

	FElevationModel ElevationModel(FTiledMap::MapzenElevation());
	if (!ElevationModel.LoadElevationData(StreetMapComponent, BuildSettings, SlowTask))
	{
		return nullptr;
	}

	TArray<uint16> ElevationData;
	if(!ElevationModel.ReprojectData(StreetMapComponent, BuildSettings, SlowTask, ElevationData))
	{
		return nullptr;
	}

	return CreateLandscape(StreetMapComponent, BuildSettings, ElevationModel.GetTransform(), ElevationData, SlowTask);
}

/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2017, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "PCCCommon.h"
#include "PCCBitstream.h"
#include "PCCBitstreamEncoder.h"
#include "PCCBitstreamEncoder.h"
#include "PCCContext.h"
#include "PCCFrameContext.h"
#include "PCCPatch.h"
#include "PCCPatchSegmenter.h"
#include "PCCVideoEncoder.h"
#include "PCCSystem.h"
#include "PCCGroupOfFrames.h"
#include "PCCPointSet.h"
#include "PCCEncoderParameters.h"
#include "PCCKdTree.h"
#include <tbb/tbb.h>
#include "PCCChrono.h"
#include "PCCEncoder.h"

uint64_t changedPixCnt;
uint64_t changedPixCnt0To1;
uint64_t changedPixCnt1To0;
uint64_t pixCnt;

using namespace std;
using namespace pcc;

std::string getEncoderConfig1L( std::string string ) {
  std::string sub    = string.substr( 0, string.find_last_of( "." ) );
  std::string result = sub + "-1L.cfg";
  return result;
}

PCCEncoder::PCCEncoder() {
#ifdef ENABLE_PAPI_PROFILING
  initPapiProfiler();
#endif
}

PCCEncoder::~PCCEncoder() {}

void PCCEncoder::setParameters( PCCEncoderParameters params ) { params_ = params; }

int PCCEncoder::encode( const PCCGroupOfFrames& sources,
                        PCCContext&             context,
                        SampleStreamVpccUnit&   ssvu,
                        PCCGroupOfFrames&       reconstructs ) {
  int    ret                                  = 0;
  size_t pointLocalReconstructionOriginal     = params_.pointLocalReconstruction_;
  size_t layerCountMinus1Original             = params_.mapCountMinus1_;
  size_t singleLayerPixelInterleavingOriginal = params_.singleMapPixelInterleaving_;
  if ( params_.nbThread_ > 0 ) { tbb::task_scheduler_init init( (int)params_.nbThread_ ); }

  params_.initializeContext( context );
  ret |= encode( sources, context, reconstructs );
  PCCBitstreamEncoder bitstreamEncoder;
#ifdef CODEC_TRACE
  setTrace( true );
  openTrace( stringFormat( "%s_GOF%u_patch_encode.txt", removeFileExtension( params_.compressedStreamPath_ ).c_str(),
                           context.getSps().getVpccParameterSetId() ) );
#endif
  createPatchFrameDataStructure( context );
#ifdef CODEC_TRACE
  closeTrace();
#endif

#ifdef BITSTREAM_TRACE
  PCCBitstream bitstream;
  bitstream.setTrace( true );
  bitstream.openTrace( removeFileExtension( params_.compressedStreamPath_ ) + "_hls_encode.txt" );
  bitstreamEncoder.setTraceFile( bitstream.getTraceFile() );
#endif
  bitstreamEncoder.setParameters( params_ );
  ret |= bitstreamEncoder.encode( context, ssvu );
#ifdef BITSTREAM_TRACE
  bitstreamEncoder.setTraceFile( NULL );
  bitstream.closeTrace();
#endif

  params_.pointLocalReconstruction_   = pointLocalReconstructionOriginal;
  params_.mapCountMinus1_             = layerCountMinus1Original;
  params_.singleMapPixelInterleaving_ = singleLayerPixelInterleavingOriginal;
  return ret;
}

int PCCEncoder::encode( const PCCGroupOfFrames& sources, PCCContext& context, PCCGroupOfFrames& reconstructs ) {
  assert( sources.size() < 256 );
  size_t atlasIndex = 0;
  if ( sources.size() == 0 ) { return 0; }
#ifdef CODEC_TRACE
  setTrace( true );
  openTrace( stringFormat( "%s_GOF%u_codec_encode.txt", removeFileExtension( params_.compressedStreamPath_ ).c_str(),
                           context.getSps().getVpccParameterSetId() ) );
#endif
  reconstructs.resize( sources.size() );
  context.resize( sources.size() );
  auto& frames = context.getFrames();
  for ( size_t i = 0; i < frames.size(); i++ ) {
    frames[i].setLosslessGeo( params_.losslessGeo_ );
    frames[i].setLosslessGeo444( params_.losslessGeo444_ );
    frames[i].setRawPatchEnabledFlag( params_.losslessGeo_ || params_.lossyMissedPointsPatch_ );
    frames[i].setUseMissedPointsSeparateVideo( params_.useMissedPointsSeparateVideo_ );
    frames[i].setGeometry3dCoordinatesBitdepth( params_.geometry3dCoordinatesBitdepth_ );
    frames[i].setGeometry2dNorminalBitdepth( params_.geometryNominal2dBitdepth_ );
    frames[i].setMaxDepth( ( 1 << params_.geometryNominal2dBitdepth_ ) - 1 );
    frames[i].setLog2PatchQuantizerSizeX( context.getLog2PatchQuantizerSizeX() );
    frames[i].setLog2PatchQuantizerSizeY( context.getLog2PatchQuantizerSizeY() );
    frames[i].setAFOC( i );
    frames[i].setRefAFOCList( context );
  }

  PCCVideoEncoder videoEncoder;
  const size_t    pointCount = sources[0].getPointCount();

  // GENERATE GEOMETRY VIDEO
  generateGeometryVideo( sources, context );

  params_.initializeContext( context );
  auto&             sps = context.getSps();
  auto&             ai  = sps.getAttributeInformation( atlasIndex );
  std::stringstream path;
  path << removeFileExtension( params_.compressedStreamPath_ ) << "_GOF" << sps.getVpccParameterSetId() << "_";

  if ( params_.globalPatchAllocation_ == 1 ) {
    performDataAdaptiveGPAMethod( context );
  } else if ( params_.globalPatchAllocation_ == 2 ) {
    doGlobalTetrisPacking( context );
  }

  if ( params_.maxNumRefAtlasFrame_ != 1 && sources.size() > 2 && params_.constrainedPack_ ) {
    adjustReferenceAtlasFrames( context );
  }

  resizeGeometryVideo( context );

  sps.setFrameWidth( atlasIndex, (uint16_t)frames[0].getWidth() );
  sps.setFrameHeight( atlasIndex, (uint16_t)frames[0].getHeight() );

  // GENERATE OCCUPANCY MAP
  generateOccupancyMap( context );

  // ENCODE OCCUPANCY MAP
  auto& videoBitstream = context.createVideoBitstream( VIDEO_OCCUPANCY );
  generateOccupancyMapVideo( sources, context );
  auto& videoOccupancyMap = context.getVideoOccupancyMap();
  videoEncoder.compress( videoOccupancyMap, path.str(), params_.occupancyMapQP_, videoBitstream,
                         params_.occupancyMapVideoEncoderConfig_, params_.videoEncoderOccupancyMapPath_, context,
                         ( params_.EOMFixBitCount_ <= 8 ) ? 1 : 2,  // nByte
                         false,                                     // use444CodecIo
                         false,                                     // use3dmv
                         8,                                         // internalBitDepth
                         false,                                     // useConversion
                         params_.keepIntermediateFiles_ );

  if ( params_.offsetLossyOM_ > 0 ) {
    changedPixCnt     = 0;
    changedPixCnt0To1 = 0;
    changedPixCnt1To0 = 0;
    pixCnt            = 0;
    modifyOccupancyMap( sources, context );
    std::cout << "Percentage of changed occupancy map values = " << ( (float)changedPixCnt * 100.0F / pixCnt )
              << std::endl;
    std::cout << "Percentage of changed occupancy map values from 0 to 1 = "
              << ( (float)changedPixCnt0To1 * 100.0F / pixCnt ) << std::endl;
    std::cout << "Percentage of changed occupancy map values from 1 to 0 = "
              << ( (float)changedPixCnt1To0 * 100.0F / pixCnt ) << std::endl;
  }

  generateBlockToPatchFromOccupancyMapVideo( context, params_.losslessGeo_, params_.lossyMissedPointsPatch_,
                                             params_.occupancyResolution_, params_.occupancyPrecision_ );

  // GEOMETRY IMAGE PADDING
  dilateGeometryVideo( sources, context );

  // Group dilation in Geometry
  if ( params_.groupDilation_ && params_.absoluteD1_ && params_.mapCountMinus1_ > 0 ) {
    geometryGroupDilation( context );
  }

  // ENCODE GEOMETRY IMAGE
  if ( params_.use3dmc_ ) { create3DMotionEstimationFiles( sources, context, path.str() ); }
  auto&  gi                      = context.getSps().getGeometryInformation( atlasIndex );
  size_t geometryVideoBitDepth   = gi.getGeometryNominal2dBitdepthMinus1() + 1;
  size_t geometryMPVideoBitDepth = gi.getGeometryNominal2dBitdepthMinus1() + 1;
  size_t nbyteGeo                = ( geometryVideoBitDepth <= 8 ) ? 1 : 2;
  size_t nbyteGeoMP              = ( geometryMPVideoBitDepth <= 8 ) ? 1 : 2;
  size_t internalBitDepth        = 10;

  if ( params_.losslessGeo_ ) { internalBitDepth = geometryVideoBitDepth; }

  if ( params_.multipleStreams_ ) {
    if ( params_.lossyMissedPointsPatch_ ) {
      std::cout << "Error: lossyMissedPointsPatch has not been implemented for absoluteD1_ = 0 as "
                   "yet. Exiting... "
                << std::endl;
      std::exit( -1 );
    }

    // Compress geometryD0
    auto& videoBitstreamD0 = context.createVideoBitstream( VIDEO_GEOMETRY_D0 );
    auto& videoGeometry    = context.getVideoGeometry();
    videoEncoder.compress(
        videoGeometry, path.str(), ( params_.geometryQP_ - 1 ), videoBitstreamD0, params_.geometryD0Config_,
        ( params_.use3dmc_ != 0 ) ? params_.videoEncoderAuxPath_ : params_.videoEncoderPath_, context,
        nbyteGeo,                                         // nbyte
        params_.losslessGeo_ && params_.losslessGeo444_,  // use444CodecIo
        params_.use3dmc_,                                 // use3dmv
        internalBitDepth,                                 // internalBitDepth
        false,                                            // useConversion
        params_.keepIntermediateFiles_ );                 // keepIntermediateFiles
    size_t sizeGeometryVideoD0 = videoBitstreamD0.size();
    std::cout << "sizeGeometryVideoD0: " << sizeGeometryVideoD0 << std::endl;
    if ( !params_.absoluteD1_ ) {
      // Form differential video geometryD1
      for ( size_t f = 0; f < frames.size(); ++f ) {
        auto& frame1 = context.getVideoGeometryD1().getFrame( f );
        predictGeometryFrame( frames[f], videoGeometry.getFrame( f ), frame1 );
        dilate3DPadding( sources[f], frames[f], frame1, videoOccupancyMap.getFrame( f ) );
      }
    }

    // Compress geometryD1
    auto& videoGeometryD1  = context.getVideoGeometryD1();
    auto& videoBitstreamD1 = context.createVideoBitstream( VIDEO_GEOMETRY_D1 );
    videoEncoder.compress(
        videoGeometryD1, path.str(), params_.geometryQP_, videoBitstreamD1, params_.geometryD1Config_,
        ( params_.use3dmc_ != 0 ) ? params_.videoEncoderAuxPath_ : params_.videoEncoderPath_, context,
        nbyteGeo,                                         // nbyte
        params_.losslessGeo_ && params_.losslessGeo444_,  // use444CodecIo
        params_.use3dmc_,                                 // use3dmv
        internalBitDepth,                                 // internalBitDepth
        false,                                            // useConversion
        params_.keepIntermediateFiles_ );

    size_t sizeGeometryVideoD1 = videoBitstreamD1.size();
    std::cout << "sizeGeometryVideoD1: " << sizeGeometryVideoD1 << std::endl;    
    std::cout << "geometryVideo ->" << ( sizeGeometryVideoD0 + sizeGeometryVideoD1 ) << "=" << sizeGeometryVideoD0
              << "+" << sizeGeometryVideoD1 << " B ("
              << ( ( sizeGeometryVideoD0 + sizeGeometryVideoD1 ) * 8.0 ) / ( 2 * frames.size() * pointCount ) << " bpp)"
              << std::endl;
  } else {
    auto& videoBitstream = context.createVideoBitstream( VIDEO_GEOMETRY );
    auto& videoGeometry  = context.getVideoGeometry();
    videoEncoder.compress(
        videoGeometry, path.str(), params_.geometryQP_, videoBitstream,
        params_.mapCountMinus1_ == 0 ? getEncoderConfig1L( params_.geometryConfig_ ) : params_.geometryConfig_,
        ( params_.use3dmc_ != 0 ) ? params_.videoEncoderAuxPath_ : params_.videoEncoderPath_, context,
        nbyteGeo,                                         // nbyte
        params_.losslessGeo_ && params_.losslessGeo444_,  // use444CodecIo
        params_.use3dmc_,                                 // use3dmv
        internalBitDepth,                                 // internalBitDepth
        false,                                            // useConversion
        params_.keepIntermediateFiles_ );                 // keepIntermediateFiles
  }

  if ( sps.getRawPatchEnabledFlag( atlasIndex ) && sps.getRawSeparateVideoPresentFlag( atlasIndex ) ) {
    auto& videoBitstreamMP = context.createVideoBitstream( VIDEO_GEOMETRY_RAW );
    generateMissedPointsGeometryVideo( context, reconstructs );
    auto& videoMPsGeometry = context.getVideoMPsGeometry();
    videoEncoder.compress( videoMPsGeometry, path.str(),
                           params_.lossyMissedPointsPatch_ ? params_.lossyMppGeoQP_ : params_.geometryQP_,
                           videoBitstreamMP, params_.geometryMPConfig_, params_.videoEncoderPath_, context,
                           nbyteGeoMP,        // nbyte
                           false,             // use444CodecIo
                           false,             // use3dmv
                           internalBitDepth,  // internalBitDepth
                           false,             // useConversion
                           params_.keepIntermediateFiles_ );
    if ( params_.lossyMissedPointsPatch_ ) { generateMissedPointsGeometryfromVideo( context, reconstructs ); }
  }

  // RECONSTRUCT POINT CLOUD GEOMETRY
  GeneratePointCloudParameters gpcParams;
  setGeneratePointCloudParameters( gpcParams, context );

  context.allocOneLayerData();
  if ( params_.pointLocalReconstruction_ ) { pointLocalReconstructionSearch( context, gpcParams ); }
  std::vector<std::vector<uint32_t>> partitions;

#if OCCUPANCY_MAP_MODEL
  upsampleOccupancyMap(context, videoOccupancyMap);

#if KEEP_OCCUPANCY_MAP_255
  std::string base_path_255 = params_.compressedStreamPath_;
  std::ofstream wf = std::ofstream(base_path_255.substr(0, base_path_255.length() - 4)+ "_ocAnchor255.yuv", std::ios::binary);
//  wf.write((char*)&videoOccupancyMap.getFrame(0).getChannel(0)[0], videoOccupancyMap.getHeight() * videoOccupancyMap.getHeight() );
  for (int i = 0; i < videoOccupancyMap.getWidth() * videoOccupancyMap.getHeight(); ++i)
  {
       uint8_t val = videoOccupancyMap.getFrame(0).getChannel(0)[i] * 255 ;
       wf.write((char*)&val, sizeof(uint8_t));
  }
  wf.close();
#endif

    PCCVideoOccupancyMap vom_org;
    vom_org.getFrames().assign(videoOccupancyMap.getFrames().begin(), videoOccupancyMap.getFrames().end());
  processIngredient(params_.modelName_, context, videoOccupancyMap);

#if KEEP_OCCUPANCY_MAP_255
 wf = std::ofstream(base_path_255.substr(0, base_path_255.length() - 4)+ "_ocModel255.yuv", std::ios::binary);
//  wf.write((char*)&videoOccupancyMap.getFrame(0).getChannel(0)[0], videoOccupancyMap.getHeight() * videoOccupancyMap.getHeight() );
  for (int i = 0; i < videoOccupancyMap.getWidth() * videoOccupancyMap.getHeight(); ++i)
  {
       uint8_t val = videoOccupancyMap.getFrame(0).getChannel(0)[i] * 255 ;
       wf.write((char*)&val, sizeof(uint8_t));
  }
  wf.close();
#endif

  refineOccupancy(vom_org, videoOccupancyMap);

#else
#if KEEP_OCCUPANCY_MAP_255
  std::string base_path_255 = params_.compressedStreamPath_;
  std::ofstream wf = std::ofstream(base_path_255.substr(0, base_path_255.length() - 4)+ "_ocRDAnchor255.yuv", std::ios::binary);
//  wf.write((char*)&videoOccupancyMap.getFrame(0).getChannel(0)[0], videoOccupancyMap.getHeight() * videoOccupancyMap.getHeight() );
  for (int i = 0; i < videoOccupancyMap.getWidth() * videoOccupancyMap.getHeight(); ++i)
  {
       uint8_t val = videoOccupancyMap.getFrame(0).getChannel(0)[i] * 255 ;
       wf.write((char*)&val, sizeof(uint8_t));
  }
  wf.close();
#endif 
#endif

generatePointCloud( reconstructs, context, gpcParams, partitions, false );
  if ( ai.getAttributeCount() > 0 ) {
    std::cout << "Texture Coding starts" << std::endl;
    const size_t mapCount             = params_.mapCountMinus1_ + 1;
    const size_t nbVideoFramePerFrame = params_.multipleStreams_ ? 1 : mapCount;
    // GENERATE ATTRIBUTE
    generateTextureVideo( sources, reconstructs, context, params_ );
    std::cout << "generate Texture Video done" << std::endl;    

    auto& videoTexture   = context.getVideoTexture();
    auto& videoTextureT1 = context.getVideoTextureT1();
    if ( !( params_.losslessGeo_ && params_.textureDilationOffLossless_ ) && params_.textureBGFill_ < 3 ) {
      // ATTRIBUTE IMAGE PADDING
      for ( size_t f = 0; f < frames.size(); ++f ) {
        using namespace std::chrono;
        pcc::chrono::Stopwatch<std::chrono::steady_clock> clockPadding;
        clockPadding.start();

        if ( params_.absoluteT1_ ) {
#if 0
          switch ( params_.textureBGFill_ ) {
            case 0:
              dilate( frames[f], videoTexture.getFrame( f * nbVideoFramePerFrame ) );
              dilate( frames[f], ( params_.multipleStreams_ ? videoTextureT1.getFrame( f )
                                                            : videoTexture.getFrame( f * nbVideoFramePerFrame + 1 ) ) );
              break;
            case 1:
              dilateSmoothedPushPull( frames[f], videoTexture.getFrame( f * nbVideoFramePerFrame ) );
              dilateSmoothedPushPull(
                  frames[f], ( params_.multipleStreams_ ? videoTextureT1.getFrame( f )
                                                        : videoTexture.getFrame( f * nbVideoFramePerFrame + 1 ) ) );
              break;
            case 2:
              dilateHarmonicBackgroundFill( frames[f], videoTexture.getFrame( f * nbVideoFramePerFrame ) );
              dilateHarmonicBackgroundFill(
                  frames[f], ( params_.multipleStreams_ ? videoTextureT1.getFrame( f )
                                                        : videoTexture.getFrame( f * nbVideoFramePerFrame + 1 ) ) );
              break;
            default: std::cout << "Warning: no texture padding applied!" << std::endl;
          }  // switch
#else
          switch ( params_.textureBGFill_ ) {
            case 0:
              for ( int mapIdx = 0; mapIdx < nbVideoFramePerFrame; mapIdx++ ) {
                if ( mapIdx == 0 ) {
                  dilate( frames[f], videoTexture.getFrame( f * nbVideoFramePerFrame ) );
                } else {  // for multiple streams, only 2 streams are being used, should re-think this
                  dilate( frames[f],
                          ( params_.multipleStreams_ ? videoTextureT1.getFrame( f )
                                                     : videoTexture.getFrame( f * nbVideoFramePerFrame + 1 ) ) );
                }
              }
              break;
            case 1:
              for ( int mapIdx = 0; mapIdx < nbVideoFramePerFrame; mapIdx++ ) {
                if ( mapIdx == 0 ) {
                  dilateSmoothedPushPull( frames[f], videoTexture.getFrame( f * nbVideoFramePerFrame ) );
                } else {  // for multiple streams, only 2 streams are being used, should re-think this
                  dilateSmoothedPushPull(
                      frames[f], ( params_.multipleStreams_ ? videoTextureT1.getFrame( f )
                                                            : videoTexture.getFrame( f * nbVideoFramePerFrame + 1 ) ) );
                }
              }
              break;
            case 2:
              for ( int mapIdx = 0; mapIdx < nbVideoFramePerFrame; mapIdx++ ) {
                if ( mapIdx == 0 ) {
                  dilateHarmonicBackgroundFill( frames[f], videoTexture.getFrame( f * nbVideoFramePerFrame ) );
                } else {  // for multiple streams, only 2 streams are being used, should re-think this
                  dilateHarmonicBackgroundFill(
                      frames[f], ( params_.multipleStreams_ ? videoTextureT1.getFrame( f )
                                                            : videoTexture.getFrame( f * nbVideoFramePerFrame + 1 ) ) );
                }
              }
              break;
            default: std::cout << "Warning: no texture padding applied!" << std::endl;
          }  // switch
#endif
          if ( mapCount > 1 && !params_.multipleStreams_ && params_.groupDilation_ ) {
            // Group dilation in texture
            auto&    frame        = frames[f];
            auto&    occupancyMap = frame.getOccupancyMap();
            auto&    width        = frame.getWidth();
            auto&    height       = frame.getHeight();
            auto&    frame1       = videoTexture.getFrame( f * nbVideoFramePerFrame );
            auto&    frame2       = videoTexture.getFrame( f * nbVideoFramePerFrame + 1 );
            uint8_t  tmp_d0, tmp_d1;
            uint32_t tmp_avg;
            for ( size_t y = 0; y < height; y++ ) {
              for ( size_t x = 0; x < width; x++ ) {
                const size_t pos = y * width + x;
                if ( occupancyMap[pos] == 0 ) {
                  for ( size_t c = 0; c < 3; c++ ) {
                    tmp_d0  = frame1.getValue( c, x, y );
                    tmp_d1  = frame2.getValue( c, x, y );
                    tmp_avg = ( (uint32_t)tmp_d0 + (uint32_t)tmp_d1 + 1 ) >> 1;
                    frame1.setValue( c, x, y, (uint8_t)tmp_avg );
                    frame2.setValue( c, x, y, (uint8_t)tmp_avg );
                  }
                }
              }
            }
          }  // groupDilation and !onelayerMode
        }    // absoluteT1
        else if ( params_.multipleStreams_ ) {
          // params_.multipleStreams_ && !absoluteT1
          switch ( params_.textureBGFill_ ) {
            case 0: dilate( frames[f], videoTexture.getFrame( f ) ); break;
            case 1: dilateSmoothedPushPull( frames[f], videoTexture.getFrame( f ) ); break;
            case 2: dilateHarmonicBackgroundFill( frames[f], videoTexture.getFrame( f ) ); break;
            default: std::cout << "Warning: no texture padding applied!" << std::endl;
          }
        }
        clockPadding.stop();
        using ms              = milliseconds;
        auto totalPaddingTime = duration_cast<ms>( clockPadding.count() ).count();
        std::cout << "Processing time (Padding [T0T1]" << f << "/" << frames.size()
                  << "): " << totalPaddingTime / 1000.0 << " s\n";
      }
    }

    // ENCODE ATTRIBUTE IMAGE
    if ( params_.multipleStreams_ ) {
      size_t nbyteAtt     = 1;
      auto&  videoTexture = context.getVideoTexture();
      // Compress textureT0
      auto& videoBitstreamT0 = context.createVideoBitstream( VIDEO_TEXTURE_T0 );
      videoEncoder.compress(
          videoTexture, path.str(), params_.textureQP_, videoBitstreamT0,
          params_.mapCountMinus1_ == 0 ? getEncoderConfig1L( params_.textureConfig_ ) : params_.textureT0Config_,
          ( params_.use3dmc_ != 0 ) ? params_.videoEncoderAuxPath_ : params_.videoEncoderPath_, context,
          nbyteAtt,                                    // nbyte
          params_.losslessGeo_,                        // use444CodecIo
          params_.use3dmc_,                            // use3dmv
          10,                                          // internalBitDepth
          !params_.losslessGeo_,                       // useConversion
          params_.keepIntermediateFiles_,              // keepIntermediateFiles
          params_.colorSpaceConversionConfig_,         // colorSpaceConversionConfig
          params_.inverseColorSpaceConversionConfig_,  // inverseColorSpaceConversionConfig
          params_.colorSpaceConversionPath_ );

      size_t sizeTextureVideoT0 = videoBitstreamT0.size();
      std::cout << "sizeTextureVideoT0: " << sizeTextureVideoT0 << std::endl;      
      // Form differential video textureT1
      auto& videoTextureT1 = context.getVideoTextureT1();
      if ( !params_.absoluteT1_ ) {
        for ( size_t f = 0; f < frames.size(); ++f ) {
          auto& frame1 = videoTextureT1.getFrame( f );
          predictTextureFrame( frames[f], videoTexture.getFrame( f ), frame1 );
          if ( !( params_.losslessGeo_ && params_.textureDilationOffLossless_ ) ) {
            switch ( params_.textureBGFill_ ) {
              case 0: dilate( frames[f], videoTextureT1.getFrame( f ) ); break;
              case 1: dilateSmoothedPushPull( frames[f], videoTextureT1.getFrame( f ) ); break;
              case 2: dilateHarmonicBackgroundFill( frames[f], videoTextureT1.getFrame( f ) ); break;
              default: std::cout << "Warning: no texture padding applied!" << std::endl;
            }
          }
        }
        std::cout << "texture prediction done " << std::endl;        
      }  //! absoluteT1

      // compress textureT1
      auto& videoBitstreamT1 = context.createVideoBitstream( VIDEO_TEXTURE_T1 );
      videoEncoder.compress(
          videoTextureT1, path.str(), params_.textureQP_ + params_.qpAdjT1_, videoBitstreamT1,
          params_.mapCountMinus1_ == 0 ? getEncoderConfig1L( params_.textureConfig_ ) : params_.textureT1Config_,
          ( params_.use3dmc_ != 0 ) ? params_.videoEncoderAuxPath_ : params_.videoEncoderPath_, context,
          nbyteAtt,                                    // nbyte
          params_.losslessGeo_,                        // use444CodecIo
          params_.use3dmc_,                            // use3dmv
          10,                                          // internalBitDepth
          !params_.losslessGeo_,                       // useConversion
          params_.keepIntermediateFiles_,              // keepIntermediateFiles
          params_.colorSpaceConversionConfig_,         // colorSpaceConversionConfig
          params_.inverseColorSpaceConversionConfig_,  // inverseColorSpaceConversionConfig
          params_.colorSpaceConversionPath_ );

      size_t sizeTextureVideoT1 = videoBitstreamT1.size();
      std::cout << "texture video ->" << ( sizeTextureVideoT0 + sizeTextureVideoT1 ) << "=" << sizeTextureVideoT0 << "+"
                << sizeTextureVideoT1 << " B ("
                << ( ( sizeTextureVideoT0 + sizeTextureVideoT1 ) * 8.0 ) / ( 2 * frames.size() * pointCount ) << " bpp)"
                << std::endl;      
    } else {
      std::cout << "texture video " << std::endl;
      auto&        videoBitstream = context.createVideoBitstream( VIDEO_TEXTURE );
      const size_t nbyteAtt       = 1;
      videoEncoder.compress(
          videoTexture, path.str(), params_.textureQP_, videoBitstream,
          params_.mapCountMinus1_ == 0 ? getEncoderConfig1L( params_.textureConfig_ ) : params_.textureConfig_,
          ( params_.use3dmc_ != 0 ) ? params_.videoEncoderAuxPath_ : params_.videoEncoderPath_, context,
          nbyteAtt,                                    // nbyte
          params_.losslessGeo_,                        // use444CodecIo
          params_.use3dmc_,                            // use3dmv
          10,                                          // internalBitDepth
          !params_.losslessGeo_,                       // useConversion
          params_.keepIntermediateFiles_,              // keepIntermediateFiles
          params_.colorSpaceConversionConfig_,         // colorSpaceConversionConfig
          params_.inverseColorSpaceConversionConfig_,  // inverseColorSpaceConversionConfig
          params_.colorSpaceConversionPath_ );         // colorSpaceConversionPath
      auto sizeTextureVideo = videoBitstream.size();
      std::cout << "texture video ->" << sizeTextureVideo << " B ("
                << ( sizeTextureVideo * 8.0 ) / ( 2 * frames.size() * pointCount ) << " bpp)" << std::endl;
    }

    if ( sps.getRawPatchEnabledFlag( atlasIndex ) && sps.getRawSeparateVideoPresentFlag( atlasIndex ) ) {
      auto& videoBitstreamMP = context.createVideoBitstream( VIDEO_TEXTURE_RAW );
      generateMissedPointsTextureVideo( context, reconstructs );  // 1. texture
      auto&        videoMPsTexture = context.getVideoMPsTexture();
      const size_t nByteAttMP      = 1;
      videoEncoder.compress( videoMPsTexture, path.str(), params_.textureQP_, videoBitstreamMP,
                             params_.textureMPConfig_, params_.videoEncoderPath_, context,
                             nByteAttMP,                                  // nbyte
                             params_.losslessGeo_,                        // use444CodecIo
                             false,                                       // use3dmv
                             10,                                          // internalBitDepth
                             !params_.losslessGeo_,                       // useConversion
                             params_.keepIntermediateFiles_,              // keepIntermediateFiles
                             params_.colorSpaceConversionConfig_,         // colorSpaceConversionConfig
                             params_.inverseColorSpaceConversionConfig_,  // inverseColorSpaceConversionConfig
                             params_.colorSpaceConversionPath_ );         // colorSpaceConversionPath
      if ( params_.lossyMissedPointsPatch_ ) { generateMissedPointsTexturefromVideo( context, reconstructs ); }
    }
  }

  if ( params_.flagGeometrySmoothing_ ) {
    if ( params_.pbfEnableFlag_ ) {
      gpcParams.pbfEnableFlag_    = true;
      gpcParams.pbfFilterSize_    = params_.pbfFilterSize_;
      gpcParams.pbfPassesCount_   = params_.pbfPassesCount_;
      gpcParams.pbfLog2Threshold_ = params_.pbfLog2Threshold_;
      for ( auto& reconstruct : reconstructs ) { reconstruct.clear(); }
      for ( auto& partition : partitions ) { partition.clear(); }
      partitions.clear();
      generatePointCloud( reconstructs, context, gpcParams, partitions, false );
    }
  }
  std::cout << "Color Point Clouds" << std::endl;
  // RECOLOR RECONSTRUCTED POINT CLOUD
  colorPointCloud( reconstructs, context, ai.getAttributeCount(), params_.colorTransform_,
                   ai.getAttributeMapAbsoluteCodingEnabledFlagList(), params_.multipleStreams_, gpcParams );

  std::cout << "Post Processing Point Clouds" << std::endl;
  //  Generate a buffer to keep unsmoothed geometry, then do geometry smoothing and transfer followed by color smoothing
  if ( gpcParams.flagGeometrySmoothing_ ) {
    if ( gpcParams.gridSmoothing_ ) {
      PCCGroupOfFrames tempFrameBuffer;
      tempFrameBuffer.resize( reconstructs.size() );
      for ( size_t i = 0; i < frames.size(); i++ ) { tempFrameBuffer[i] = reconstructs[i]; }
      smoothPointCloudPostprocess( reconstructs, context, params_.colorTransform_, gpcParams, partitions );
      for ( size_t i = 0; i < frames.size(); i++ ) {
        // The parameters for the attribute transfer are still fixed (may wish to make them user input/more flexible)
        // These are different attribute transfer functions
        if ( params_.postprocessSmoothingFilter_ == 1 ) {
          tempFrameBuffer[i].transferColors( reconstructs[i], int32_t( 0 ), sps.getLosslessGeo() == 1, 8, 1, 1, 1, 1, 0,
                                             4, 4, 1000, 1000, 1000, 1000 );
        } else if ( params_.postprocessSmoothingFilter_ == 2 ) {
          tempFrameBuffer[i].transferColorWeight( reconstructs[i], 0.1 );
        } else if ( params_.postprocessSmoothingFilter_ == 3 ) {
          tempFrameBuffer[i].transferColorsFilter3( reconstructs[i], int32_t( 0 ), sps.getLosslessGeo() == 1 );
        }
      }
    }
  }
  //    This function does the color smoothing that is usually done in colorPointCloud
  if ( gpcParams.flagColorSmoothing_ ) { colorSmoothing( reconstructs, context, params_.colorTransform_, gpcParams ); }
  if ( !params_.keepIntermediateFiles_ && params_.use3dmc_ ) { remove3DMotionEstimationFiles( path.str() ); }
#ifdef CODEC_TRACE
  setTrace( false );
  closeTrace();
#endif
  return 0;
}

void PCCEncoder::printMap( std::vector<bool> img, const size_t sizeU, const size_t sizeV ) {
  std::cout << std::endl;
  std::cout << "PrintMap size = " << sizeU << " x " << sizeV << std::endl;
  for ( size_t v = 0; v < sizeV; ++v ) {
    for ( size_t u = 0; u < sizeU; ++u ) { std::cout << ( img[v * sizeU + u] ? 'X' : '.' ); }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

void PCCEncoder::printMapTetris( std::vector<bool> img,
                                 const size_t      sizeU,
                                 const size_t      sizeV,
                                 std::vector<int>  horizon ) {
  std::cout << std::endl;
  std::cout << "PrintMap size = " << sizeU << " x " << sizeV << std::endl;
  for ( int v = 0; v < sizeV; ++v ) {
    for ( int u = 0; u < sizeU; ++u ) {
      if ( v == horizon[u] )
        std::cout << ( img[v * sizeU + u] ? 'U' : 'O' );
      else
        std::cout << ( img[v * sizeU + u] ? 'X' : '.' );
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

static const std::vector<int32_t> kernel = {12, 28, 12, 28, 96, 28, 12, 28, 12};

template <typename T>
T PCCEncoder::limit( T x, T minVal, T maxVal ) {
  return ( x < minVal ) ? minVal : ( x > maxVal ? maxVal : x );
}

void PCCEncoder::preFilterOccupancyMap( PCCImageOccupancyMap& image, size_t kwidth, size_t kheight ) {
  if ( kwidth == 0 ) kwidth = sqrt( kernel.size() );
  if ( kheight == 0 ) kheight = sqrt( kernel.size() );

  const size_t width  = image.getWidth();
  const size_t height = image.getHeight();

  const size_t kCenterW = kwidth / 2;
  const size_t kCenterH = kheight / 2;

  const auto imageTemp( image );
  int        val;
  for ( size_t v = 0; v < height; v++ ) {
    for ( size_t u = 0; u < width; u++ ) {
      val = 0;
      for ( size_t n = 0; n < kheight; n++ ) {
        size_t nn = kheight - 1 - n;
        for ( size_t m = 0; m < kwidth; m++ ) {
          size_t mm = kwidth - 1 - m;
          size_t q  = nn * kwidth + mm;

          size_t i = limit<int32_t>( int32_t( u + kCenterW - mm ), 0, width - 1 );
          size_t j = limit<int32_t>( int32_t( v + kCenterH - nn ), 0, height - 1 );
          val += (double)imageTemp.getValue( 0, i, j ) * kernel[q];
        }
      }
      image.setValue( 0, u, v, ( uint8_t )( val >> 8 ) );
    }
  }
}

bool PCCEncoder::generateOccupancyMapVideo( const PCCGroupOfFrames& sources, PCCContext& context ) {
  auto& videoOccupancyMap = context.getVideoOccupancyMap();
  bool  ret               = true;
  videoOccupancyMap.resize( sources.size() );
  for ( size_t f = 0; f < sources.size(); ++f ) {
    auto&                 contextFrame = context.getFrames()[f];
    PCCImageOccupancyMap& videoFrame   = videoOccupancyMap.getFrame( f );
    ret &= generateOccupancyMapVideo( contextFrame.getWidth(), contextFrame.getHeight(), contextFrame.getOccupancyMap(),
                                      videoFrame );
  }
  return ret;
}

bool PCCEncoder::generateOccupancyMapVideo( const size_t           imageWidth,
                                            const size_t           imageHeight,
                                            std::vector<uint32_t>& occupancyMap,
                                            PCCImageOccupancyMap&  videoFrameOccupancyMap ) {
  const size_t   blockSize0  = params_.occupancyResolution_ / params_.occupancyPrecision_;
  const size_t   pointCount0 = blockSize0 * blockSize0;
  vector<bool>   block0;
  vector<size_t> bestRuns;
  vector<size_t> runs;
  block0.resize( pointCount0 );
  size_t       videoFrameOccupancyMapSizeU = imageWidth / params_.occupancyPrecision_;
  size_t       videoFrameOccupancyMapSizeV = imageHeight / params_.occupancyPrecision_;
  const size_t blockToPatchWidth           = imageWidth / params_.occupancyResolution_;
  const size_t blockToPatchHeight          = imageHeight / params_.occupancyResolution_;

  if ( !params_.enhancedDeltaDepthCode_ ) {
    videoFrameOccupancyMap.resize( videoFrameOccupancyMapSizeU, videoFrameOccupancyMapSizeV );
    for ( size_t v0 = 0; v0 < blockToPatchHeight; ++v0 ) {
      for ( size_t u0 = 0; u0 < blockToPatchWidth; ++u0 ) {
        size_t fullCount = 0;
        for ( size_t v1 = 0; v1 < blockSize0; ++v1 ) {
          const size_t v2 = v0 * params_.occupancyResolution_ + v1 * params_.occupancyPrecision_;
          for ( size_t u1 = 0; u1 < blockSize0; ++u1 ) {
            const size_t u2     = u0 * params_.occupancyResolution_ + u1 * params_.occupancyPrecision_;
            bool         isFull = false;
            for ( size_t v3 = 0; v3 < params_.occupancyPrecision_ && !isFull; ++v3 ) {
              for ( size_t u3 = 0; u3 < params_.occupancyPrecision_ && !isFull; ++u3 ) {
                isFull |= ( occupancyMap[( v2 + v3 ) * imageWidth + u2 + u3] > 0 );
              }
            }
            block0[v1 * blockSize0 + u1] = isFull;
            fullCount += isFull;
            /*for ( size_t v3 = 0; v3 < params_.occupancyPrecision_; ++v3 ) {
              for ( size_t u3 = 0; u3 < params_.occupancyPrecision_; ++u3 ) {
                occupancyMap[( v2 + v3 ) * imageWidth + u2 + u3] = isFull; -> does not update the occupancy map, this
            will be done somewhere else
            }
            }*/
          }
        }
        for ( size_t iterBlockV = 0; iterBlockV < blockSize0; iterBlockV++ ) {
          for ( size_t iterBlockU = 0; iterBlockU < blockSize0; iterBlockU++ ) {
            uint8_t pixel = block0[iterBlockV * blockSize0 + iterBlockU];
            if ( pixel > 0 ) { pixel = ( params_.offsetLossyOM_ > 0 ) ? params_.offsetLossyOM_ : 1; }
            size_t videoFrameU = u0 * blockSize0 + iterBlockU;
            size_t videoFrameV = v0 * blockSize0 + iterBlockV;
            videoFrameOccupancyMap.setValue( 0, videoFrameU, videoFrameV, pixel );
          }
        }
      }
    }
  } else {
    videoFrameOccupancyMap.resize( imageWidth, imageHeight );
    for ( size_t v = 0; v < imageHeight; v++ ) {
      for ( size_t u = 0; u < imageWidth; u++ ) {
        size_t i      = v * imageWidth + u;
        size_t symbol = occupancyMap[i];
        if ( symbol < 0 ) { symbol = 0; }
        if ( symbol > 1023 ) { symbol = 1023; }
        videoFrameOccupancyMap.setValue( 0, u, v, symbol );
      }
    }
  }

  if ( params_.prefilterLossyOM_ ) { preFilterOccupancyMap( videoFrameOccupancyMap, 3, 3 ); }

  return true;
}

bool PCCEncoder::modifyOccupancyMap( const PCCGroupOfFrames& sources, PCCContext& context ) {
  std::ofstream oFile;

  if ( params_.keepIntermediateFiles_ ) { oFile.open( "occupancyMap.rgb", std::ios::binary ); }

  auto& videoOccupancyMap = context.getVideoOccupancyMap();
  bool  ret               = true;
  for ( size_t f = 0; f < sources.size(); ++f ) {
    auto&                 contextFrame = context.getFrames()[f];
    PCCImageOccupancyMap& videoFrame   = videoOccupancyMap.getFrame( f );
    ret &= modifyOccupancyMap( contextFrame.getWidth(), contextFrame.getHeight(), contextFrame.getOccupancyMap(),
                               videoFrame, oFile );
  }

  if ( params_.keepIntermediateFiles_ ) { oFile.close(); }

  return ret;
}

bool PCCEncoder::modifyOccupancyMap( const size_t           imageWidth,
                                     const size_t           imageHeight,
                                     std::vector<uint32_t>& occupancyMap,
                                     PCCImageOccupancyMap&  videoFrameOccupancyMap,
                                     std::ofstream&         ofile ) {
  const size_t numSubBlksV = imageHeight / params_.occupancyPrecision_;
  const size_t numSubBlksH = imageWidth / params_.occupancyPrecision_;

  // const size_t threshold = OM_OFFSET / 2;

  std::vector<uint32_t> newOccupancyMap;
  newOccupancyMap.resize( imageWidth * imageHeight );
  char tmpC;

  for ( size_t v0 = 0; v0 < numSubBlksV; ++v0 ) {
    const size_t v1 = v0 * params_.occupancyPrecision_;
    for ( size_t u0 = 0; u0 < numSubBlksH; ++u0 ) {
      const size_t u1    = u0 * params_.occupancyPrecision_;
      uint8_t      pixel = videoFrameOccupancyMap.getValue( 0, u0, v0 );
      for ( size_t v2 = 0; v2 < params_.occupancyPrecision_; v2++ ) {
        for ( size_t u2 = 0; u2 < params_.occupancyPrecision_; u2++ ) {
          size_t index = ( v1 + v2 ) * imageWidth + u1 + u2;
          pixCnt++;
          if ( pixel <= params_.thresholdLossyOM_ ) {
            newOccupancyMap[index] = 0;
          } else {
            newOccupancyMap[index] = 1;
          }

          if ( occupancyMap[index] != newOccupancyMap[index] ) {
            changedPixCnt++;
            if ( occupancyMap[index] == 0 ) {
              changedPixCnt0To1++;
              if ( params_.keepIntermediateFiles_ ) {
                tmpC = (char)255;
                ofile.write( &tmpC, 1 );
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
              }
            } else {
              changedPixCnt1To0++;
              if ( params_.keepIntermediateFiles_ ) {
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
                tmpC = (char)255;
                ofile.write( &tmpC, 1 );
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
              }
            }
          } else {
            if ( occupancyMap[index] == 0 ) {
              if ( params_.keepIntermediateFiles_ ) {
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
                tmpC = (char)0;
                ofile.write( &tmpC, 1 );
              }
            } else {
              if ( params_.keepIntermediateFiles_ ) {
                tmpC = (char)255;
                ofile.write( &tmpC, 1 );
                tmpC = (char)255;
                ofile.write( &tmpC, 1 );
                tmpC = (char)255;
                ofile.write( &tmpC, 1 );
              }
            }
          }
          occupancyMap[index] = newOccupancyMap[index];
        }
      }
    }
  }

  return true;
}

void PCCEncoder::modifyOccupancyMapEDD( PCCFrameContext& frame ) {
  auto& occupancyMap     = frame.getOccupancyMap();
  auto& fullOccupancyMap = frame.getFullOccupancyMap();
  auto& width            = frame.getWidth();
  auto& height           = frame.getHeight();
  occupancyMap.resize( width * height, 0 );
  if ( !params_.absoluteD1_ || !params_.absoluteT1_ ) fullOccupancyMap.resize( width * height, 0 );
  for ( auto& patch : frame.getPatches() ) {
    for ( size_t v = 0; v < patch.getSizeV(); ++v ) {
      for ( size_t u = 0; u < patch.getSizeU(); ++u ) {
        const size_t  p       = v * patch.getSizeU() + u;
        const int16_t d       = patch.getDepth( 0 )[p];
        const int16_t eddCode = patch.getDepthEnhancedDeltaD()[p];
        size_t        x, y;
        auto          indx = patch.patch2Canvas( u, v, width, height, x, y );
        assert( x < width && y < height );
        const size_t d0 = d;
        if ( params_.mapCountMinus1_ == 0 ) {  // one layer
          bool updateOccupancy = ( d < infiniteDepth ) && ( occupancyMap[indx] == 1 );
          if ( updateOccupancy ) {
            const size_t N      = params_.EOMFixBitCount_;
            int16_t      symbol = ( 1 << N ) - 1;
            symbol -= eddCode;
            // uint16_t nbBits = 0;
            //          for ( uint16_t i = 0; i < N; i++ )
            //            if ( eddCode & ( 1 << i ) ) numOfEddpoints++;  // nbBits++;
            if ( symbol < 0 ) symbol = 0;
            occupancyMap[indx] += symbol;
          }
        } else {
          const size_t d1      = patch.getDepth( 1 )[p];
          bool updateOccupancy = ( ( d < infiniteDepth ) && ( occupancyMap[indx] == 1 ) && ( ( d1 - d0 ) > 1 ) );
          if ( updateOccupancy ) {
            uint16_t bits       = d1 - d0 - 1;
            uint16_t eddExtract = eddCode & ( ~( ( ~0 ) << bits ) );
            uint16_t symbol     = ( ( ( 1 << bits ) - 1 ) - eddExtract );
            occupancyMap[indx] += symbol;
          }
        }
      }
    }  // u
  }    // v

  if ( !params_.absoluteD1_ || !params_.absoluteT1_ ) { fullOccupancyMap = occupancyMap; }
}

void PCCEncoder::adjustReferenceAtlasFrames( PCCContext& context ) {
  auto& frames = context.getFrames();
  for ( size_t frameIndex = 2; frameIndex < frames.size(); frameIndex++ ) {
    std::cout << std::endl << ":::::---- adjusting reference frames for frame " << frameIndex << std::endl;
    auto&                 frame        = context[frameIndex];
    double                dMinListDist = 0;
    std::vector<PCCPatch> bestPatchList;
    size_t                bestListIdx   = 0;
    double                dTempListDist = 0;
    std::vector<PCCPatch> tempPatchList;
    for ( size_t listIdx = 0; listIdx < frame.getNumOfRefAtlasFrameList(); listIdx++ ) {
      dTempListDist = 0;
      tempPatchList.clear();
      double dTempListDist = adjustReferenceAtlasFrame( context, frame, listIdx, tempPatchList );
      if ( dTempListDist > dMinListDist ) {
        dMinListDist  = dTempListDist;
        bestListIdx   = listIdx;
        bestPatchList = tempPatchList;
      }
    }
    frame.setActiveRefAtlasFrameIndex( bestListIdx );
    frame.getPatches() = bestPatchList;
  }  // frame
}

double PCCEncoder::adjustReferenceAtlasFrame( PCCContext&            context,
                                              PCCFrameContext&       frame,
                                              size_t                 listIndex,
                                              std::vector<PCCPatch>& tempPatchList ) {
  PCCBitstream tempBitStream;
  auto         curPatches    = frame.getPatches();
  size_t       curPatchCount = curPatches.size();
  if ( curPatches.empty() ) { return -1; }
  vector<double> maxIOUList;
  maxIOUList.resize( curPatchCount, -1.0f );
  double sumMaxIOU = 0;
  // initialization
  size_t maxU0 = 0, maxV0 = 0, maxU1 = 0, maxV1 = 0, maxD1 = 0, maxDD;
  for ( size_t patchIdx = 0; patchIdx < curPatchCount; patchIdx++ ) {
    maxU0 = ( std::max )( maxU0, curPatches[patchIdx].getU0() );
    maxV0 = ( std::max )( maxV0, curPatches[patchIdx].getV0() );
    maxU1 = ( std::max )( maxU0, curPatches[patchIdx].getU1() );
    maxV1 = ( std::max )( maxU0, curPatches[patchIdx].getV1() );
    maxD1 = ( std::max )( maxU0, curPatches[patchIdx].getD1() );
    maxDD = ( std::max )( maxU0, curPatches[patchIdx].getSizeD() );
  }
  uint8_t bitMaxU0 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxU0 + 1 ) ) );
  uint8_t bitMaxV0 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxV0 + 1 ) ) );
  uint8_t bitMaxU1 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxU1 + 1 ) ) );
  uint8_t bitMaxV1 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxV1 + 1 ) ) );
  uint8_t bitMaxD1 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxD1 + 1 ) ) );
  uint8_t bitMaxDD = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxDD + 1 ) ) );

  const size_t max3DCoordinate = 1 << ( params_.geometry3dCoordinatesBitdepth_ );
  for ( size_t curId = 0; curId < curPatchCount; curId++ ) {
    auto& curPatch = curPatches[curId];
    // intra
    float initSize = tempBitStream.size();
    tempBitStream.write( uint32_t( curPatch.getU0() ), bitMaxU0 );
    tempBitStream.write( uint32_t( curPatch.getV0() ), bitMaxV0 );
    tempBitStream.writeSvlc(
        int32_t( curId == 0 ? curPatch.getSizeU0() : curPatch.getSizeU0() - curPatches[curId - 1].getSizeU0() ) );
    tempBitStream.writeSvlc(
        int32_t( curId == 0 ? curPatch.getSizeV0() : curPatch.getSizeV0() - curPatches[curId - 1].getSizeV0() ) );
    tempBitStream.write( uint32_t( curPatch.getU1() ), bitMaxU1 );
    tempBitStream.write( uint32_t( curPatch.getV1() ), bitMaxV1 );
    tempBitStream.write( uint32_t( curPatch.getD1() ), bitMaxD1 );
    tempBitStream.write( uint32_t( curPatch.getSizeD() ), bitMaxDD );
    tempBitStream.write( uint32_t( curPatch.getViewId() ), 3 );
    if ( params_.useEightOrientations_ ) {
      tempBitStream.write( curPatch.getPatchOrientation(), 3 );
    } else {
      tempBitStream.write( curPatch.getPatchOrientation(), 1 );
    }
    if ( curPatch.getAxisOfAdditionalPlane() ) {
      tempBitStream.write( uint32_t( curPatch.getAxisOfAdditionalPlane() ), 1 );
    }
    float bitCostIntraA = tempBitStream.size();

    // inter
    if ( curPatch.getBestMatchIdx() != -1 ) {
      size_t refPOC   = frame.getRefAFOC( listIndex, 0 );
      auto&  refPatch = context[refPOC].getPatch( curPatch.getBestMatchIdx() );
      tempBitStream.writeSvlc( int32_t( (int64_t)curPatch.getBestMatchIdx() - curId ) );  // approx
      tempBitStream.writeUvlc( int32_t( 0 ) );
      tempBitStream.writeSvlc( int32_t( curPatch.getU0() - refPatch.getU0() ) );
      tempBitStream.writeSvlc( int32_t( curPatch.getV0() - refPatch.getV0() ) );
      tempBitStream.writeSvlc( int32_t( curPatch.getSizeU0() - refPatch.getSizeU0() ) );
      tempBitStream.writeSvlc( int32_t( curPatch.getSizeV0() - refPatch.getSizeV0() ) );
      tempBitStream.writeSvlc( int32_t( curPatch.getU1() - refPatch.getU1() ) );
      tempBitStream.writeSvlc( int32_t( curPatch.getV1() - refPatch.getV1() ) );

      size_t        quantDD  = curPatch.getSizeD() == 0 ? 0 : ( ( curPatch.getSizeD() - 1 ) / params_.minLevel_ + 1 );
      size_t        prevQDD  = refPatch.getSizeD() == 0 ? 0 : ( ( refPatch.getSizeD() - 1 ) / params_.minLevel_ + 1 );
      const int64_t delta_dd = ( (int64_t)quantDD ) - ( (int64_t)prevQDD );
      tempBitStream.writeSvlc( int32_t( delta_dd ) );  // se(v)

      int32_t delta_d1 = 0;
      if ( curPatch.getProjectionMode() == 0 || !params_.absoluteD1_ ) {
        delta_d1 = ( ( curPatch.getD1() / params_.minLevel_ ) - ( refPatch.getD1() / params_.minLevel_ ) );
      } else {
        if ( curPatch.getAxisOfAdditionalPlane() == 0 ) {
          delta_d1 = ( max3DCoordinate - curPatch.getD1() ) / params_.minLevel_ -
                     ( max3DCoordinate - refPatch.getD1() ) / params_.minLevel_;
        } else {
          delta_d1 = ( ( max3DCoordinate << 1 ) - curPatch.getD1() ) / params_.minLevel_ -
                     ( ( max3DCoordinate << 1 ) - refPatch.getD1() ) / params_.minLevel_;
        }
      }
      tempBitStream.writeSvlc( delta_d1 );
    }
    float bitCostInterA = ( curPatch.getBestMatchIdx() != -1 ) ? tempBitStream.size() : 0;
    float bitCostInter  = bitCostInterA - initSize;
    float bitCostIntra  = bitCostIntraA - initSize;

    maxIOUList[curId] = 1 / bitCostIntra;
    curPatch.setBestMatchIdx( -1 );
  }

  // loop over refPicture in the list
  size_t sizeOfList = frame.getRefAFOCListSize( listIndex );
  for ( size_t refIdx = 0; refIdx < sizeOfList; refIdx++ ) {
    size_t refPOC     = frame.getRefAFOC( listIndex, refIdx );
    auto&  refPatches = context.getFrame( refPOC ).getPatches();
    for ( size_t refPatchId = 0; refPatchId < refPatches.size(); refPatchId++ ) {
      // bestOrderPatches.clear();
      auto& refPatch   = refPatches[refPatchId];
      float maxIou     = 0.0f;
      int   bestCurIdx = -1;
      for ( size_t curId = 0; curId < curPatchCount; curId++ ) {
        auto& curPatch     = curPatches[curId];
        bool  bMatchingRef = refPatch.getViewId() == curPatch.getViewId() &&
                            refPatch.getPatchOrientation() == curPatch.getPatchOrientation();
        if ( bMatchingRef ) {
          float initSize = tempBitStream.size();
          tempBitStream.writeSvlc( int32_t( (int64_t)refPatchId - curId ) );  // approx
          tempBitStream.writeUvlc( int32_t( refIdx ) );
          tempBitStream.writeSvlc( int32_t( curPatch.getU0() - refPatch.getU0() ) );
          tempBitStream.writeSvlc( int32_t( curPatch.getV0() - refPatch.getV0() ) );
          tempBitStream.writeSvlc( int32_t( curPatch.getSizeU0() - refPatch.getSizeU0() ) );
          tempBitStream.writeSvlc( int32_t( curPatch.getSizeV0() - refPatch.getSizeV0() ) );
          tempBitStream.writeSvlc( int32_t( curPatch.getU1() - refPatch.getU1() ) );
          tempBitStream.writeSvlc( int32_t( curPatch.getV1() - refPatch.getV1() ) );
          size_t quantDD = curPatch.getSizeD() == 0 ? 0 : ( ( curPatch.getSizeD() - 1 ) / params_.minLevel_ + 1 );
          size_t prevQDD = refPatch.getSizeD() == 0 ? 0 : ( ( refPatch.getSizeD() - 1 ) / params_.minLevel_ + 1 );
          const int64_t delta_dd = ( (int64_t)quantDD ) - ( (int64_t)prevQDD );
          tempBitStream.writeSvlc( int32_t( delta_dd ) );  // se(v)

          int32_t delta_d1 = 0;
          if ( curPatch.getProjectionMode() == 0 || !params_.absoluteD1_ ) {
            delta_d1 = ( ( curPatch.getD1() / params_.minLevel_ ) - ( refPatch.getD1() / params_.minLevel_ ) );
          } else {
            if ( curPatch.getAxisOfAdditionalPlane() == 0 ) {
              delta_d1 = ( max3DCoordinate - curPatch.getD1() ) / params_.minLevel_ -
                         ( max3DCoordinate - refPatch.getD1() ) / params_.minLevel_;
            } else {
              delta_d1 = ( ( max3DCoordinate << 1 ) - curPatch.getD1() ) / params_.minLevel_ -
                         ( ( max3DCoordinate << 1 ) - refPatch.getD1() ) / params_.minLevel_;
            }
          }
          tempBitStream.writeSvlc( delta_d1 );

          //      float bitCostInterA=tempBitStream.size();
          float bitCostInter = tempBitStream.size() - initSize;
          // float bitCostIntra=bitCostIntraA-bitCostInterA;
          // float areaOverlap=computeIOU( refRect, curRect );
          float iou = 1 / bitCostInter;

          if ( iou > maxIou ) {
            maxIou     = iou;
            bestCurIdx = curId;
          }

        }  // end of if (patch.viewId == cpatch.viewId).
      }
      if ( bestCurIdx >= 0 && maxIou > maxIOUList[bestCurIdx] ) {
        curPatches[bestCurIdx].setBestMatchIdx( refPatchId );    // the matched patch id in preivious frame.
        curPatches[bestCurIdx].setRefAtlasFrameIndex( refIdx );  // the matched patch id in preivious frame.
        curPatches[bestCurIdx].setPatchType( (uint8_t)PATCH_MODE_P_INTER );
        maxIOUList[bestCurIdx] = maxIou;
      }
    }  // refPatch

  }  // refIdx

  // no reordering!
  size_t numInterPredictedPatches = 0;
  for ( size_t patchIdx = 0; patchIdx < curPatchCount; patchIdx++ ) {
    if ( curPatches[patchIdx].getBestMatchIdx() != PCC_UNDEFINED_INDEX ) {
      curPatches[patchIdx].setPatchType( (uint8_t)PATCH_MODE_P_INTER );
      numInterPredictedPatches++;
      sumMaxIOU += maxIOUList[patchIdx];
    } else {
      curPatches[patchIdx].setPatchType( (uint8_t)PATCH_MODE_P_INTRA );
    }

    tempPatchList.push_back( curPatches[patchIdx] );
  }

  frame.setNumMatchedPatches( numInterPredictedPatches );
  return sumMaxIOU;
}

void PCCEncoder::spatialConsistencyPack( PCCFrameContext& frame,
                                         PCCFrameContext& prevFrame,
                                         int              safeguard,
                                         bool             enablePointCloudPartitioning ) {
  auto& width       = frame.getWidth();
  auto& height      = frame.getHeight();
  auto& patches     = frame.getPatches();
  auto& prevPatches = prevFrame.getPatches();
  if ( patches.empty() ) { return; }
  std::sort( patches.begin(), patches.end() );
  int    id             = 0;
  size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t occupancySizeV =
      ( std::max )( params_.minimumImageHeight_ / params_.occupancyResolution_, patches[0].getSizeV0() );
  vector<PCCPatch> matchedPatches, tmpPatches, newOrderPatches;
  matchedPatches.clear();
  newOrderPatches.clear();
  float thresholdIOU = 0.2f;

  // main loop.
  for ( auto& patch : prevPatches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    id++;
    float maxIou  = 0.0f;
    int   bestIdx = -1, cId = 0;
    for ( auto& cpatch : patches ) {
      if ( ( patch.getViewId() == cpatch.getViewId() ) && ( cpatch.getBestMatchIdx() == InvalidPatchIndex ) &&
           ( patch.getLodScaleX() == cpatch.getLodScaleX() && patch.getLodScaleY() == cpatch.getLodScaleY() ) ) {
        Rect  rect  = Rect( patch.getU1(), patch.getV1(), patch.getSizeU(), patch.getSizeV() );
        Rect  crect = Rect( cpatch.getU1(), cpatch.getV1(), cpatch.getSizeU(), cpatch.getSizeV() );
        float iou   = computeIOU( rect, crect );
        if ( iou > maxIou ) {
          maxIou  = iou;
          bestIdx = cId;
        }
      }  // end of if (patch.viewId == cpatch.viewId).
      cId++;
    }
    if ( maxIou > thresholdIOU ) {
      // store the best match index
      patches[bestIdx].setBestMatchIdx( id - 1 );  // the matched patch id in previous frame.
      patches[bestIdx].setPatchType( (uint8_t)PATCH_MODE_P_INTER );
      matchedPatches.push_back( patches[bestIdx] );
    }
  }

  // generate new patch order.
  newOrderPatches = matchedPatches;
  for ( auto patch : patches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    if ( patch.getBestMatchIdx() == InvalidPatchIndex ) {
      patch.setPatchType( (uint8_t)PATCH_MODE_P_INTRA );  // P_TYPE_INTRA
      newOrderPatches.push_back( patch );
    }
  }

  frame.setNumMatchedPatches( matchedPatches.size() );
  // remove the below logs when useless.
  patches = newOrderPatches;
  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }

  int numTilesHor = params_.numTilesHor_;
  int tileWidth   = occupancySizeU / numTilesHor;
  int tileHeight  = int( tileWidth * params_.tileHeightToWidthRatio_ );
  std::cout << "tile size  = " << tileWidth << " x " << tileHeight << std::endl;
  occupancySizeV = ( occupancySizeV >= tileHeight ) ? occupancySizeV : tileHeight;

  width  = occupancySizeU * params_.occupancyResolution_;
  height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  std::vector<bool> occupancyMap;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  if ( !enablePointCloudPartitioning ) {
    for ( auto& patch : patches ) {
      assert( patch.getSizeU0() <= occupancySizeU );
      assert( patch.getSizeV0() <= occupancySizeV );
      bool  locationFound = false;
      auto& occupancy     = patch.getOccupancy();
      while ( !locationFound ) {
        patch.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;  // only one orientation is allowed
        for ( size_t v = 0; v <= occupancySizeV && !locationFound; ++v ) {
          for ( size_t u = 0; u <= occupancySizeU && !locationFound; ++u ) {
            patch.getU0() = u;
            patch.getV0() = v;
            if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                            safeguard ) ) {
              locationFound = true;
            }
          }
        }
        if ( !locationFound ) {
          occupancySizeV *= 2;
          occupancyMap.resize( occupancySizeU * occupancySizeV );
        }
      }
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
        const size_t v = patch.getV0() + v0;
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
          const size_t u = patch.getU0() + u0;
          if ( params_.lowDelayEncoding_ )
            occupancyMap[v * occupancySizeU + u] = true;
          else
            occupancyMap[v * occupancySizeU + u] =
                occupancyMap[v * occupancySizeU + u] || occupancy[v0 * patch.getSizeU0() + u0];
        }
      }

      height          = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      width           = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
      // print(occupancyMap, occupancySizeU, occupancySizeV);
    }
  } else {
    std::vector<Tile> tilesNotAvailable;
    int               numROIs                        = params_.numROIs_;
    int               lastOccupiedTileIndex          = -1;
    int               lastOccupiedTileIndexByPrevROI = -1;
    int               numTilesAvailable;
    // loop over ROIs
    for ( size_t roiIndex = 0; roiIndex < numROIs; ++roiIndex ) {
      tilesNotAvailable.clear();
      if ( roiIndex > 0 ) {
        lastOccupiedTileIndexByPrevROI = lastOccupiedTileIndex;
        for ( int tileIndex = 0; tileIndex <= lastOccupiedTileIndexByPrevROI; ++tileIndex ) {
          Tile tile;
          tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
          tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
          tile.maxU = tile.minU + tileWidth - 1;
          tile.maxV = tile.minV + tileHeight - 1;
          tilesNotAvailable.push_back( tile );
        }
      }
      // loop over patches of current ROI
      for ( auto& patch : patches ) {
        if ( roiIndex != patch.getRoiIndex() ) { continue; }
        assert( patch.getSizeU0() <= occupancySizeU );
        assert( patch.getSizeV0() <= occupancySizeV );
        bool  locationFound = false;
        auto& occupancy     = patch.getOccupancy();
        while ( !locationFound ) {
          patch.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;  // only one orientation is allowed
          numTilesAvailable           = ceil( double( occupancySizeV ) / double( tileHeight ) ) * numTilesHor;
          for ( int tileIndex = lastOccupiedTileIndexByPrevROI + 1; tileIndex < numTilesAvailable && !locationFound;
                ++tileIndex ) {
            Tile tile;
            tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
            tile.maxU = tile.minU + tileWidth - 1;
            tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
            tile.maxV = tile.minV + tileHeight - 1;
            for ( size_t v = 0; v <= occupancySizeV && !locationFound; ++v ) {
              for ( size_t u = 0; u <= occupancySizeU && !locationFound; ++u ) {
                patch.getU0()        = u;
                patch.getV0()        = v;
                bool tileIsAvailable = true;
                if ( roiIndex > 0 ) {
                  for ( auto& tile : tilesNotAvailable ) {
                    if ( ( tile.minU <= u && u <= tile.maxU ) && ( tile.minV <= v && v <= tile.maxV ) &&
                         ( !locationFound ) ) {
                      tileIsAvailable = false;
                      break;
                    }
                  }
                }
                if ( tileIsAvailable ) {
                  if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                  params_.lowDelayEncoding_, safeguard, tile ) ) {
                    locationFound = true;
                    if ( tileIndex > lastOccupiedTileIndex ) { lastOccupiedTileIndex = tileIndex; }
                    std::cout << "ROI-" << roiIndex + 1 << " patch-" << patch.getIndex() << " fitted in tile-"
                              << tileIndex + 1 << "/" << numTilesAvailable << "-----[" << tile.minU << "," << tile.maxU
                              << "][" << tile.minV << "," << tile.maxV << "]"
                              << " +-+-+-+-+-(NOT MATCHED patch)-+-+-+-+-+" << std::endl;
                  }
                }
              }
            }
          }
          if ( !locationFound ) {
            occupancySizeV *= 2;
            occupancyMap.resize( occupancySizeU * occupancySizeV );
          }
        }
        for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
          const size_t v = patch.getV0() + v0;
          for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
            const size_t u = patch.getU0() + u0;
            occupancyMap[v * occupancySizeU + u] =
                occupancyMap[v * occupancySizeU + u] || occupancy[v0 * patch.getSizeU0() + u0];
          }
        }

        height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
        width  = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
        // print(occupancyMap, occupancySizeU, occupancySizeV);
      }  // patch loop
      printMap( occupancyMap, occupancySizeU, occupancySizeV );
    }  // ROI loop
  }

  if ( frame.getNumberOfMissedPointsPatches() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
    packMissedPointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  } else {
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  }
  if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
    packEOMTexturePointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  }
  if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }

  std::cout << "actualImageSize " << width << " x" << height << std::endl;
}

void PCCEncoder::spatialConsistencyPackFlexible( PCCFrameContext& frame,
                                                 PCCFrameContext& prevFrame,
                                                 int              safeguard,
                                                 bool             enablePointCloudPartitioning ) {
  auto& width   = frame.getWidth();
  auto& height  = frame.getHeight();
  auto& patches = frame.getPatches();

  auto& prevPatches = prevFrame.getPatches();
  if ( patches.empty() ) { return; }
  std::sort( patches.begin(), patches.end(), []( PCCPatch& a, PCCPatch& b ) { return a.gt( b ); } );
  int              id             = 0;
  size_t           occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t           occupancySizeV = ( std::max )( patches[0].getSizeU0(), patches[0].getSizeV0() );
  vector<PCCPatch> matchedPatches, tmpPatches;
  matchedPatches.clear();
  float  thresholdIOU    = 0.2f;
  size_t bestRefFrameIdx = 0;
  // main loop.
  for ( auto& patch : prevPatches ) {
    id++;
    float maxIou  = 0.0f;
    int   bestIdx = -1, cId = 0;
    for ( auto& cpatch : patches ) {
      if ( ( patch.getViewId() == cpatch.getViewId() ) && ( cpatch.getBestMatchIdx() == InvalidPatchIndex ) &&
           ( patch.getLodScaleX() == cpatch.getLodScaleX() && patch.getLodScaleY() == cpatch.getLodScaleY() ) ) {
        patch.setPatchType( (uint8_t)PATCH_MODE_P_INTRA );
        Rect  rect  = Rect( patch.getU1(), patch.getV1(), patch.getSizeU(), patch.getSizeV() );
        Rect  crect = Rect( cpatch.getU1(), cpatch.getV1(), cpatch.getSizeU(), cpatch.getSizeV() );
        float iou   = computeIOU( rect, crect );
        if ( iou > maxIou ) {
          maxIou  = iou;
          bestIdx = cId;
        }
      }  // end of if (patch.viewId == cpatch.viewId).
      cId++;
    }

    if ( maxIou > thresholdIOU ) {
      // store the best match index
      patches[bestIdx].setBestMatchIdx( id - 1 );  // the matched patch id in preivious frame.
      patches[bestIdx].setPatchType( (uint8_t)PATCH_MODE_P_INTER );
      patches[bestIdx].setRefAtlasFrameIndex( bestRefFrameIdx );
      matchedPatches.push_back( patches[bestIdx] );
    }
  }

  // generate new patch order.
  vector<PCCPatch> newOrderPatches = matchedPatches;

  for ( auto patch : patches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    if ( patch.getBestMatchIdx() == InvalidPatchIndex ) {
      patch.setPatchType( (uint8_t)PATCH_MODE_P_INTRA );
      newOrderPatches.push_back( patch );
    }
  }
  frame.setNumMatchedPatches( matchedPatches.size() );

  // remove the below logs when useless.
  if ( printDetailedInfo ) {
    std::cout << "patches.size:" << patches.size() << ",reOrderedPatches.size:" << newOrderPatches.size()
              << ",matchedpatches.size:" << frame.getNumMatchedPatches() << std::endl;
  }
  patches = newOrderPatches;
  if ( printDetailedInfo ) {
    std::cout << "Patch order:" << std::endl;
    for ( auto& patch : patches ) {
      std::cout << "Patch[" << patch.getIndex() << "]=(" << patch.getSizeU0() << "," << patch.getSizeV0() << ")"
                << std::endl;
    }
  }

  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }

  int numTilesHor = params_.numTilesHor_;
  int tileWidth   = occupancySizeU / numTilesHor;
  int tileHeight  = int( tileWidth * params_.tileHeightToWidthRatio_ );
  std::cout << "tile size  = " << tileWidth << " x " << tileHeight << std::endl;
  occupancySizeV = ( occupancySizeV >= tileHeight ) ? occupancySizeV : tileHeight;

  width  = occupancySizeU * params_.occupancyResolution_;
  height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  vector<int> orientation_vertical = {
      PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
  vector<int> orientation_horizontal = {
      PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};  // favoring horizontal orientations (that should be
                                                             // rotated)
  int               numOrientations = params_.useEightOrientations_ ? 8 : 2;
  std::vector<bool> occupancyMap;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  if ( !params_.enablePointCloudPartitioning_ ) {
    for ( auto& patch : patches ) {
      assert( patch.getSizeU0() <= occupancySizeU );
      assert( patch.getSizeV0() <= occupancySizeV );
      bool  locationFound = false;
      auto& occupancy     = patch.getOccupancy();
      while ( !locationFound ) {
        if ( patch.getBestMatchIdx() != InvalidPatchIndex ) {
          patch.getPatchOrientation() = prevPatches[patch.getBestMatchIdx()].getPatchOrientation();
          // try to place on the same position as the matched patch
          patch.getU0() = prevPatches[patch.getBestMatchIdx()].getU0();
          patch.getV0() = prevPatches[patch.getBestMatchIdx()].getV0();
          if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_ ) ) {
            locationFound = true;
            if ( printDetailedInfo ) {
              std::cout << "Maintained orientation " << patch.getPatchOrientation() << " for matched patch "
                        << patch.getIndex() << " in the same position (" << patch.getU0() << "," << patch.getV0() << ")"
                        << std::endl;
            }
          }
          // if the patch couldn't fit, try to fit the patch in the top left position
          for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
            for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
              patch.getU0() = u;
              patch.getV0() = v;
              if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                              safeguard ) ) {
                locationFound = true;
                if ( printDetailedInfo ) {
                  std::cout << "Maintained orientation " << patch.getPatchOrientation() << " for matched patch "
                            << patch.getIndex() << " (" << u << "," << v << ")" << std::endl;
                }
              }
            }
          }
        } else {
          // best effort
          for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
            for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
              patch.getU0() = u;
              patch.getV0() = v;
              for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound; orientationIdx++ ) {
                if ( patch.getSizeU0() > patch.getSizeV0() ) {
                  patch.getPatchOrientation() = orientation_horizontal[orientationIdx];
                } else {
                  patch.getPatchOrientation() = orientation_vertical[orientationIdx];
                }
                if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                                safeguard ) ) {
                  locationFound = true;
                  if ( printDetailedInfo ) {
                    std::cout << "Orientation " << patch.getPatchOrientation() << " selected for unmatched patch "
                              << patch.getIndex() << " (" << u << "," << v << ")" << std::endl;
                  }
                }
              }
            }
          }
        }
        if ( !locationFound ) {
          occupancySizeV *= 2;
          occupancyMap.resize( occupancySizeU * occupancySizeV );
        }
      }
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
          int coord = patch.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
          if ( params_.lowDelayEncoding_ )
            occupancyMap[coord] = true;
          else
            occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
        }
      }
      if ( !( patch.isPatchDimensionSwitched() ) ) {
        height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
        width  = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
      } else {
        height = ( std::max )( height, ( patch.getV0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
        width  = ( std::max )( width, ( patch.getU0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeU0() ) );
      }
    }
  } else {
    std::vector<Tile> tilesNotAvailable;
    int               numROIs                        = params_.numROIs_;
    int               lastOccupiedTileIndex          = -1;
    int               lastOccupiedTileIndexByPrevROI = -1;
    int               numTilesAvailable;
    // loop over ROIs
    for ( size_t roiIndex = 0; roiIndex < numROIs; ++roiIndex ) {
      tilesNotAvailable.clear();
      if ( roiIndex > 0 ) {
        lastOccupiedTileIndexByPrevROI = lastOccupiedTileIndex;
        for ( int tileIndex = 0; tileIndex <= lastOccupiedTileIndexByPrevROI; ++tileIndex ) {
          Tile tile;
          tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
          tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
          tile.maxU = tile.minU + tileWidth - 1;
          tile.maxV = tile.minV + tileHeight - 1;
          tilesNotAvailable.push_back( tile );
        }
      }
      // loop over patches of current ROI
      for ( auto& patch : patches ) {
        if ( roiIndex != patch.getRoiIndex() ) { continue; }
        assert( patch.getSizeU0() <= occupancySizeU );
        assert( patch.getSizeV0() <= occupancySizeV );
        bool  locationFound = false;
        auto& occupancy     = patch.getOccupancy();
        while ( !locationFound ) {
          if ( patch.getBestMatchIdx() != InvalidPatchIndex ) {
            patch.getPatchOrientation() = prevPatches[patch.getBestMatchIdx()].getPatchOrientation();
            // try to place on the same position as the matched patch
            patch.getU0() = prevPatches[patch.getBestMatchIdx()].getU0();
            patch.getV0() = prevPatches[patch.getBestMatchIdx()].getV0();
            // try to place the matched patch into the tiles occupied by the current ROI so far
            int firstTileIndexOfCurrentROI;
            int lastTileIndexOfCurrentROI;
            if ( lastOccupiedTileIndex == -1 ) {
              firstTileIndexOfCurrentROI = 0;
              lastTileIndexOfCurrentROI  = 0;
            } else {
              firstTileIndexOfCurrentROI = lastOccupiedTileIndexByPrevROI + 1;
              lastTileIndexOfCurrentROI  = lastOccupiedTileIndex;
            }
            //
            for ( int tileIndex = firstTileIndexOfCurrentROI; tileIndex <= lastTileIndexOfCurrentROI && !locationFound;
                  ++tileIndex ) {
              Tile tile;
              tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
              tile.maxU = tile.minU + tileWidth - 1;
              tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
              tile.maxV = tile.minV + tileHeight - 1;

              if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                              safeguard, tile ) ) {
                locationFound = true;
                std::cout << "ROI-" << roiIndex + 1 << " patch-" << patch.getIndex() << " fitted in tile-"
                          << tileIndex + 1 << "/" << numTilesAvailable << "-----[" << tile.minU << "," << tile.maxU
                          << "][" << tile.minV << "," << tile.maxV << "]"
                          << " +-+-+-+-+-(MATCHED patch placed on the same position)-+-+-+-+-+" << std::endl;
                if ( printDetailedInfo ) {
                  std::cout << "Maintained orientation " << patch.getPatchOrientation() << " for matched patch "
                            << patch.getIndex() << " in the same position (" << patch.getU0() << "," << patch.getV0()
                            << ")" << std::endl;
                }
              }
            }
            // if the patch couldn't fit, try to fit the patch in the top left position
            numTilesAvailable = ceil( double( occupancySizeV ) / double( tileHeight ) ) * numTilesHor;
            for ( int tileIndex = lastOccupiedTileIndexByPrevROI + 1; tileIndex < numTilesAvailable && !locationFound;
                  ++tileIndex ) {
              Tile tile;
              tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
              tile.maxU = tile.minU + tileWidth - 1;
              tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
              tile.maxV = tile.minV + tileHeight - 1;
              for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
                for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
                  patch.getU0()        = u;
                  patch.getV0()        = v;
                  bool tileIsAvailable = true;
                  if ( roiIndex > 0 ) {
                    for ( auto& tile : tilesNotAvailable ) {
                      if ( ( tile.minU <= u && u <= tile.maxU ) && ( tile.minV <= v && v <= tile.maxV ) &&
                           ( !locationFound ) ) {
                        tileIsAvailable = false;
                        break;
                      }
                    }
                  }
                  if ( tileIsAvailable ) {
                    if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                    params_.lowDelayEncoding_, safeguard, tile ) ) {
                      locationFound = true;
                      if ( tileIndex > lastOccupiedTileIndex ) { lastOccupiedTileIndex = tileIndex; }
                      std::cout << "ROI-" << roiIndex + 1 << " patch-" << patch.getIndex() << " fitted in tile-"
                                << tileIndex + 1 << "/" << numTilesAvailable << "-----[" << tile.minU << ","
                                << tile.maxU << "][" << tile.minV << "," << tile.maxV << "]"
                                << " +-+-+-+-+-(MATCHED patch NOT placed on the same position)-+-+-+-+-+" << std::endl;
                      if ( printDetailedInfo ) {
                        std::cout << "Maintained orientation " << patch.getPatchOrientation() << " for matched patch "
                                  << patch.getIndex() << " (" << u << "," << v << ")" << std::endl;
                      }
                    }
                  }
                }
              }
            }
          } else {
            // best effort
            numTilesAvailable = ceil( double( occupancySizeV ) / double( tileHeight ) ) * numTilesHor;
            for ( int tileIndex = lastOccupiedTileIndexByPrevROI + 1; tileIndex < numTilesAvailable && !locationFound;
                  ++tileIndex ) {
              Tile tile;
              tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
              tile.maxU = tile.minU + tileWidth - 1;
              tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
              tile.maxV = tile.minV + tileHeight - 1;
              for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
                for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
                  patch.getU0()        = u;
                  patch.getV0()        = v;
                  bool tileIsAvailable = true;
                  if ( roiIndex > 0 ) {
                    for ( auto& tile : tilesNotAvailable ) {
                      if ( ( tile.minU <= u && u <= tile.maxU ) && ( tile.minV <= v && v <= tile.maxV ) &&
                           ( !locationFound ) ) {
                        tileIsAvailable = false;
                        break;
                      }
                    }
                  }
                  if ( tileIsAvailable ) {
                    for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound;
                          orientationIdx++ ) {
                      if ( patch.getSizeU0() > patch.getSizeV0() ) {
                        patch.getPatchOrientation() = orientation_horizontal[orientationIdx];
                      } else {
                        patch.getPatchOrientation() = orientation_vertical[orientationIdx];
                      }
                      if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                      params_.lowDelayEncoding_, safeguard, tile ) ) {
                        locationFound = true;
                        if ( tileIndex > lastOccupiedTileIndex ) { lastOccupiedTileIndex = tileIndex; }
                        std::cout << "ROI-" << roiIndex + 1 << " patch-" << patch.getIndex() << " fitted in tile-"
                                  << tileIndex + 1 << "/" << numTilesAvailable << "-----[" << tile.minU << ","
                                  << tile.maxU << "][" << tile.minV << "," << tile.maxV << "]"
                                  << " +-+-+-+-+-(NOT MATCHED patch)-+-+-+-+-+" << std::endl;
                        if ( printDetailedInfo ) {
                          std::cout << "Orientation " << patch.getPatchOrientation() << " selected for unmatched patch "
                                    << patch.getIndex() << " (" << u << "," << v << ")" << std::endl;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
          if ( !locationFound ) {
            occupancySizeV *= 2;
            occupancyMap.resize( occupancySizeU * occupancySizeV );
          }
        }  // while loop
        for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
          for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
            int coord           = patch.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
            occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
          }
        }
        if ( !( patch.isPatchDimensionSwitched() ) ) {
          height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
          width  = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
        } else {
          height = ( std::max )( height, ( patch.getV0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
          width  = ( std::max )( width, ( patch.getU0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeU0() ) );
        }
      }  // patch loop
      printMap( occupancyMap, occupancySizeU, occupancySizeV );
    }  // ROI loop
  }

  if ( frame.getNumberOfMissedPointsPatches() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
    packMissedPointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  } else {
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  }
  if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
    packEOMTexturePointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  }
  if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  std::cout << "actualImageSize " << width << " x " << height << std::endl;
}

void PCCEncoder::spatialConsistencyPackTetris( PCCFrameContext& frame, PCCFrameContext& prevFrame, int safeguard ) {
  auto& width   = frame.getWidth();
  auto& height  = frame.getHeight();
  auto& patches = frame.getPatches();

  auto& prevPatches = prevFrame.getPatches();
  if ( patches.empty() ) { return; }
  std::sort( patches.begin(), patches.end(), []( PCCPatch& a, PCCPatch& b ) { return a.gt( b ); } );
  int              id             = 0;
  size_t           occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t           occupancySizeV = ( std::max )( patches[0].getSizeU0(), patches[0].getSizeV0() );
  vector<PCCPatch> matchedPatches, tmpPatches;
  matchedPatches.clear();
  float thresholdIOU = 0.2;

  // main loop.
  for ( auto& patch : prevPatches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    id++;
    float maxIou  = 0.0;
    int   bestIdx = -1, cId = 0;
    for ( auto& cpatch : patches ) {
      if ( ( patch.getViewId() == cpatch.getViewId() ) && ( cpatch.getBestMatchIdx() == -1 ) &&
           ( patch.getLodScaleX() == cpatch.getLodScaleX() && patch.getLodScaleY() == cpatch.getLodScaleY() ) ) {
        Rect  rect  = Rect( patch.getU1(), patch.getV1(), patch.getSizeU(), patch.getSizeV() );
        Rect  crect = Rect( cpatch.getU1(), cpatch.getV1(), cpatch.getSizeU(), cpatch.getSizeV() );
        float iou   = computeIOU( rect, crect );
        if ( iou > maxIou ) {
          maxIou  = iou;
          bestIdx = cId;
        }
      }  // end of if (patch.viewId == cpatch.viewId).
      cId++;
    }

    if ( maxIou > thresholdIOU ) {
      // store the best match index
      patches[bestIdx].setBestMatchIdx( id - 1 );  // the matched patch id in preivious frame.
      patches[bestIdx].setPatchType( (uint8_t)PATCH_MODE_P_INTER );
      matchedPatches.push_back( patches[bestIdx] );
    }
  }

  // generate new patch order.
  vector<PCCPatch> newOrderPatches = matchedPatches;

  for ( auto patch : patches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    if ( patch.getBestMatchIdx() == -1 ) { newOrderPatches.push_back( patch ); }
  }
  frame.setNumMatchedPatches( matchedPatches.size() );

  // remove the below logs when useless.
  if ( printDetailedInfo ) {
    std::cout << "patches.size:" << patches.size() << ",reOrderedPatches.size:" << newOrderPatches.size()
              << ",matchedpatches.size:" << frame.getNumMatchedPatches() << std::endl;
  }
  patches = newOrderPatches;
  if ( printDetailedInfo ) {
    std::cout << "Patch order:" << std::endl;
    for ( auto& patch : patches ) {
      std::cout << "Patch[" << patch.getIndex() << "]=(" << patch.getSizeU0() << "," << patch.getSizeV0() << ")"
                << std::endl;
    }
  }

  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }

  width  = occupancySizeU * params_.occupancyResolution_;
  height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  std::vector<bool> occupancyMap;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  std::vector<int> horizon;
  horizon.resize( occupancySizeU, 0 );

  for ( auto& patch : patches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    auto& occupancy = patch.getOccupancy();
    // getting the horizons using the rotation 0 position
    std::vector<int> top_horizon;
    std::vector<int> bottom_horizon;
    std::vector<int> right_horizon;
    std::vector<int> left_horizon;
    patch.get_patch_horizons( top_horizon, bottom_horizon, right_horizon, left_horizon );

    bool locationFound = false;
    while ( !locationFound ) {
      int    best_wasted_space = ( std::numeric_limits<int>::max )();
      size_t best_u, best_v;
      int    best_orientation;
      if ( patch.getBestMatchIdx() != -1 ) {
        patch.getPatchOrientation() = prevPatches[patch.getBestMatchIdx()].getPatchOrientation();
        best_orientation            = patch.getPatchOrientation();
        // spiral search to find the closest available position
        int x   = 0;
        int y   = 0;
        int end = ( std::max )( occupancySizeU, occupancySizeV ) * ( std::max )( occupancySizeU, occupancySizeV ) * 4;
        for ( int i = 0; i < end && !locationFound; ++i ) {
          // Translate coordinates and mask them out.
          int xp = x + prevPatches[patch.getBestMatchIdx()].getU0();
          int yp = y + prevPatches[patch.getBestMatchIdx()].getV0();
          if ( printDetailedInfo ) std::cout << "Testing position (" << xp << ',' << yp << ')' << std::endl;
          if ( xp >= 0 && xp < occupancySizeU && yp >= 0 && yp < occupancySizeV ) {
            patch.getU0() = xp;
            patch.getV0() = yp;
            if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                            safeguard ) ) {
              locationFound = true;
              best_u        = xp;
              best_v        = yp;
              if ( printDetailedInfo )
                std::cout << "Maintained orientation " << patch.getPatchOrientation() << " for matched patch "
                          << patch.getIndex() << " in new position (" << xp << "," << yp << ")" << std::endl;
            }
          }
          if ( abs( x ) <= abs( y ) && ( x != y || x >= 0 ) )
            x += ( ( y >= 0 ) ? 1 : -1 );
          else
            y += ( ( x >= 0 ) ? -1 : 1 );
        }
      } else {
        vector<int> orientation_values = {
            PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
            PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
            PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
        int numOrientations = params_.useEightOrientations_ ? 8 : 2;
        // tetris packing
        for ( size_t u = 0; u < occupancySizeU; ++u ) {
          for ( size_t v = 0; v < occupancySizeV; ++v ) {
            patch.getU0() = u;
            patch.getV0() = v;
            for ( size_t orientationIdx = 0; orientationIdx < numOrientations; orientationIdx++ ) {
              patch.getPatchOrientation() = orientation_values[orientationIdx];
              if ( !patch.isPatchLocationAboveHorizon( horizon, top_horizon, bottom_horizon, right_horizon,
                                                       left_horizon ) ) {
                if ( printDetailedInfo )
                  std::cout << "(" << u << "," << v << "|" << patch.getPatchOrientation() << ") above horizon"
                            << std::endl;
                continue;
              }
              if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                              safeguard ) ) {
                // now calculate the wasted space
                int wasted_space =
                    patch.calculate_wasted_space( horizon, top_horizon, bottom_horizon, right_horizon, left_horizon );
                if ( wasted_space < best_wasted_space ) {
                  best_wasted_space = wasted_space;
                  best_u            = u;
                  best_v            = v;
                  best_orientation  = patch.getPatchOrientation();
                  locationFound     = true;
                }
              }
            }
          }
        }
      }
      if ( !locationFound ) {
        occupancySizeV *= 2;
        occupancyMap.resize( occupancySizeU * occupancySizeV );
      } else {
        // select the best position and orientation
        patch.getU0()               = best_u;
        patch.getV0()               = best_v;
        patch.getPatchOrientation() = best_orientation;
        if ( printDetailedInfo )
          std::cout << "Selected position (" << best_u << "," << best_v << ") and orientation " << best_orientation
                    << std::endl;
        // update the horizon
        patch.update_horizon( horizon, top_horizon, bottom_horizon, right_horizon, left_horizon );
        // debugging
        if ( printDetailedInfo ) {
          std::cout << "New Horizon :[";
          for ( int i = 0; i < occupancySizeU; i++ ) { std::cout << horizon[i] << ","; }
          std::cout << "]" << std::endl;
        }
      }
    }
    for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
      for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
        int coord = patch.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
        if ( params_.lowDelayEncoding_ )
          occupancyMap[coord] = true;
        else
          occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
      }
    }
    if ( !( patch.isPatchDimensionSwitched() ) ) {
      height          = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      width           = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
    } else {
      height          = ( std::max )( height, ( patch.getV0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      width           = ( std::max )( width, ( patch.getU0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeU0() ) );
    }
    if ( printDetailedInfo ) { printMapTetris( occupancyMap, occupancySizeU, occupancySizeV, horizon ); }
  }

  if ( frame.getNumberOfMissedPointsPatches() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
    packMissedPointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  } else {
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  }
  if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
    packEOMTexturePointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  }
  if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  std::cout << "actualImageSize " << width << " x " << height << std::endl;
}

// GTP - GLOBAL PATCH PACKING
void PCCEncoder::findMatchesForGlobalTetrisPacking( PCCFrameContext& frame, PCCFrameContext& prevFrame ) {
  auto& patches     = frame.getPatches();
  auto& prevPatches = prevFrame.getPatches();
  if ( patches.empty() ) { return; }
  // sort the patches so that the first match is done with the largest patch first.
  std::sort( patches.begin(), patches.end(), []( PCCPatch& a, PCCPatch& b ) { return a.gt( b ); } );

  if ( &frame == &prevFrame ) {
    if ( printDetailedInfo ) {
      for ( int patchIdx = 0; patchIdx < patches.size(); patchIdx++ ) {
        auto& patch = patches[patchIdx];
        std::cout << "Sorted Patch[" << patchIdx << "]->";
        patch.getU0() = 0;
        patch.getV0() = 0;
        patch.print();
      }
    }
    // no point in doing matching, this is the same frame
    return;
  }

  vector<PCCPatch> matchedPatches;
  int              id = 0;
  matchedPatches.clear();
  float thresholdIOU = 0.2f;
  // main loop.
  for ( auto& patch : prevPatches ) {
    id++;
    float maxIou  = 0.0f;
    int   bestIdx = -1, cId = 0;
    for ( auto& cpatch : patches ) {
      if ( ( patch.getViewId() == cpatch.getViewId() ) && ( cpatch.getBestMatchIdx() == InvalidPatchIndex ) &&
           ( patch.getLodScaleX() == cpatch.getLodScaleX() && patch.getLodScaleY() == cpatch.getLodScaleY() ) ) {
        Rect  rect  = Rect( patch.getU1(), patch.getV1(), patch.getSizeU(), patch.getSizeV() );
        Rect  crect = Rect( cpatch.getU1(), cpatch.getV1(), cpatch.getSizeU(), cpatch.getSizeV() );
        float iou   = computeIOU( rect, crect );
        if ( iou > maxIou ) {
          maxIou  = iou;
          bestIdx = cId;
        }
      }  // end of if (patch.viewId == cpatch.viewId).
      cId++;
    }
    if ( maxIou > thresholdIOU ) {
      // checking the size of the matched patches
      auto&  curPatch = patches[bestIdx];
      double area1    = curPatch.getSizeU0() * curPatch.getSizeV0();
      double area2    = patch.getSizeU0() * patch.getSizeV0();
      if ( ( ( area1 / area2 ) < params_.globalPackingStrategyThreshold_ ) ||
           ( ( area2 / area1 ) < params_.globalPackingStrategyThreshold_ ) ) {
        // this seems like an unlike mismatch, will break the chain here
        if ( printDetailedInfo ) {
          std::cout << "Removing the match because areas are too different:" << std::endl;
          std::cout << "elem.ID =" << curPatch.getIndex() << std::endl;
          std::cout << "elem.sizeU0 =" << curPatch.getSizeU0() << std::endl;
          std::cout << "elem.sizeV0 =" << curPatch.getSizeV0() << std::endl;
          std::cout << "area =" << area1 << std::endl;
          std::cout << "previous_elem.ID =" << patch.getIndex() << std::endl;
          std::cout << "elem.sizeU0 =" << patch.getSizeU0() << std::endl;
          std::cout << "elem.sizeV0 =" << patch.getSizeV0() << std::endl;
          std::cout << "area =" << area2 << std::endl;
        }
      } else {
        // store the best match index
        patches[bestIdx].setBestMatchIdx( id - 1 );  // the matched patch id in previous frame.
        matchedPatches.push_back( patches[bestIdx] );
      }
    }
  }
  frame.setNumMatchedPatches( matchedPatches.size() );

  vector<PCCPatch> newOrderPatches = matchedPatches;
  for ( auto patch : patches ) {
    if ( patch.getBestMatchIdx() == InvalidPatchIndex ) { newOrderPatches.push_back( patch ); }
  }
  frame.setNumMatchedPatches( matchedPatches.size() );

  patches = newOrderPatches;
  if ( printDetailedInfo ) {
    for ( int patchIdx = 0; patchIdx < patches.size(); patchIdx++ ) {
      auto& patch = patches[patchIdx];
      if ( patchIdx < frame.getNumMatchedPatches() )
        std::cout << "Matched (refPatch[" << patches[patchIdx].getBestMatchIdx()
                  << "]=" << prevPatches[patches[patchIdx].getBestMatchIdx()].getIndex() << ") Patch[" << patchIdx
                  << "]->";
      else
        std::cout << "Unmatched Patch[" << patchIdx << "]->";
      patch.getU0() = 0;
      patch.getV0() = 0;
      patch.print();
    }
  }
}

void PCCEncoder::doGlobalTetrisPacking( PCCContext& context ) {
  struct doubleLinkedPatchElement {
    pcc::PCCPatch*    elem;
    int32_t           nextElemPos;
    int32_t           prevElemPos;
    int32_t           weight;
    pcc::PCCPatch     globalElem;
    std::vector<bool> globalOccupancyMap;

    doubleLinkedPatchElement() : elem( nullptr ), nextElemPos( -1 ), prevElemPos( -1 ), weight( 0 ) {}
    doubleLinkedPatchElement( pcc::PCCPatch* patch ) :
        elem( patch ),
        nextElemPos( -1 ),
        prevElemPos( patch->getBestMatchIdx() ),
        weight( patch->getBestMatchIdx() >= 0 ? 1 : 0 ) {}
    bool gt( const doubleLinkedPatchElement& rhs ) {
      // setting the largest dimension
      if ( weight > rhs.weight ) {
        return true;
      } else {
        if ( weight < rhs.weight ) {
          return false;
        } else
          return elem->gt( rhs.elem[0] );
      }
    }
    bool gt2( const doubleLinkedPatchElement& rhs ) {
      // setting the largest dimension
      float volume       = weight * ( globalOccupancyMap.size() );
      float volume_right = rhs.weight * ( rhs.globalOccupancyMap.size() );
      if ( volume > volume_right ) {
        return true;
      } else {
        if ( volume < volume_right ) {
          return false;
        } else
          return elem->gt( rhs.elem[0] );
      }
    }
  };

  std::vector<std::vector<doubleLinkedPatchElement>> patchMatrix;
  std::vector<std::vector<int32_t>>                  patchMatrixSortedIndexes;

  // creating the doubled linked list
  patchMatrix.resize( context.size() );
  patchMatrixSortedIndexes.resize( context.size() );
  auto& frames  = context.getFrames();
  int   gofSize = ( params_.globalPackingStrategyGOF_ == 0 ) ? frames.size() : params_.globalPackingStrategyGOF_;
  int   numGof  = ( frames.size() + gofSize / 2 ) / gofSize;
  for ( int gofIdx = 0; gofIdx < numGof; gofIdx++ ) {
    int frStart = ( gofIdx ) * ( gofSize );
    int frEnd   = ( gofIdx + 1 ) * ( gofSize );
    if ( gofIdx + 1 == numGof ) frEnd = frames.size();
    for ( size_t frameIdx = frStart; frameIdx < frEnd; frameIdx++ ) {
      auto& patches = frames[frameIdx].getPatches();
      patchMatrix[frameIdx].resize( patches.size() );
      patchMatrixSortedIndexes[frameIdx].resize( patches.size() );
      for ( size_t patchIdx = 0; patchIdx < patches.size(); patchIdx++ ) {
        doubleLinkedPatchElement elem( &patches[patchIdx] );
        if ( frameIdx == frStart ) {
          elem.prevElemPos = -1;  // break the link between GOFs
          if ( params_.globalPackingStrategyReset_ ) { elem.elem->setBestMatchIdx( InvalidPatchIndex ); }
        }
        patchMatrix[frameIdx][patchIdx]              = elem;
        patchMatrixSortedIndexes[frameIdx][patchIdx] = patchIdx;
      }
    }
    // now go from back to front and update the nextElemPos and the weight
    for ( int frameIdx = frEnd - 1; frameIdx >= frStart; frameIdx-- ) {
      for ( size_t patchIdx = 0; patchIdx < patchMatrix[frameIdx].size(); patchIdx++ ) {
        // update the list
        if ( patchMatrix[frameIdx][patchIdx].prevElemPos >= 0 )
          patchMatrix[frameIdx - 1][patchMatrix[frameIdx][patchIdx].prevElemPos].nextElemPos = patchIdx;
        // update the weight, the global occupancy map, and the patch size
        if ( patchMatrix[frameIdx][patchIdx].nextElemPos >= 0 ) {
          // new weight
          patchMatrix[frameIdx][patchIdx].weight +=
              patchMatrix[frameIdx + 1][patchMatrix[frameIdx][patchIdx].nextElemPos].weight + 1;
          // new occupancy map
          size_t curU1                  = patchMatrix[frameIdx][patchIdx].elem->getU1();
          size_t curV1                  = patchMatrix[frameIdx][patchIdx].elem->getV1();
          size_t curSizeU0              = patchMatrix[frameIdx][patchIdx].elem->getSizeU0();
          size_t curSizeV0              = patchMatrix[frameIdx][patchIdx].elem->getSizeV0();
          size_t curOccupancyResolution = patchMatrix[frameIdx][patchIdx].elem->getOccupancyResolution();

          auto&  nextGlobalElem = patchMatrix[frameIdx + 1][patchMatrix[frameIdx][patchIdx].nextElemPos].globalElem;
          size_t nextU1         = nextGlobalElem.getU1();
          size_t nextV1         = nextGlobalElem.getV1();
          size_t nextSizeU0     = nextGlobalElem.getSizeU0();
          size_t nextSizeV0     = nextGlobalElem.getSizeV0();
          size_t nextOccupancyResolution = curOccupancyResolution;

          auto& curGlobalElem   = patchMatrix[frameIdx][patchIdx].globalElem;
          curGlobalElem.getU1() = min( curU1, nextU1 );
          curGlobalElem.getV1() = min( curV1, nextV1 );

          curGlobalElem.getSizeU0() = ( max( ( curSizeU0 - 1 ) * curOccupancyResolution + curU1,
                                             ( nextSizeU0 - 1 ) * nextOccupancyResolution + nextU1 ) -
                                        curGlobalElem.getU1() ) /
                                          curOccupancyResolution +
                                      1;
          curGlobalElem.getSizeV0() = ( max( ( curSizeV0 - 1 ) * curOccupancyResolution + curV1,
                                             ( nextSizeV0 - 1 ) * nextOccupancyResolution + nextV1 ) -
                                        curGlobalElem.getV1() ) /
                                          curOccupancyResolution +
                                      1;

          curGlobalElem.getOccupancy().resize( curGlobalElem.getSizeU0() * curGlobalElem.getSizeV0(), false );
          // copy the global occupancy map from next patch
          for ( size_t v = 0; v < nextSizeV0; v++ ) {
            for ( size_t u = 0; u < nextSizeU0; u++ ) {
              size_t posGlobal =
                  ( nextU1 - curGlobalElem.getU1() ) / curOccupancyResolution + u +
                  curGlobalElem.getSizeU0() * ( v + ( nextV1 - curGlobalElem.getV1() ) / curOccupancyResolution );
              size_t posNext                          = u + nextSizeU0 * ( v );
              curGlobalElem.getOccupancy()[posGlobal] = nextGlobalElem.getOccupancy()[posNext];
            }
          }
          // copy the global occupancy map from current patch
          for ( size_t v = 0; v < curSizeV0; v++ ) {
            for ( size_t u = 0; u < curSizeU0; u++ ) {
              size_t posGlobal =
                  ( curU1 - curGlobalElem.getU1() ) / curOccupancyResolution + u +
                  curGlobalElem.getSizeU0() * ( v + ( curV1 - curGlobalElem.getV1() ) / curOccupancyResolution );
              size_t posCur                           = u + curSizeU0 * ( v );
              curGlobalElem.getOccupancy()[posGlobal] = curGlobalElem.getOccupancy()[posGlobal] ||
                                                        patchMatrix[frameIdx][patchIdx].elem->getOccupancy()[posCur];
            }
          }
          curGlobalElem.getOccupancyResolution() = curOccupancyResolution;

          if ( printDetailedInfo ) {
            std::cout << "Next Global Element" << std::endl;
            std::cout << "patchMatrix[" << frameIdx + 1 << "][" << patchMatrix[frameIdx][patchIdx].nextElemPos
                      << "].globalElem.U1 =" << nextU1 << std::endl;
            std::cout << "patchMatrix[" << frameIdx + 1 << "][" << patchMatrix[frameIdx][patchIdx].nextElemPos
                      << "].globalElem.V1 =" << nextV1 << std::endl;
            printMap( nextGlobalElem.getOccupancy(), nextGlobalElem.getSizeU0(), nextGlobalElem.getSizeV0() );

            std::cout << "Current Element" << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.ID =" << patchMatrix[frameIdx][patchIdx].elem->getIndex() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.U1 =" << patchMatrix[frameIdx][patchIdx].elem->getU1() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.V1 =" << patchMatrix[frameIdx][patchIdx].elem->getV1() << std::endl;
            printMap( patchMatrix[frameIdx][patchIdx].elem->getOccupancy(),
                      patchMatrix[frameIdx][patchIdx].elem->getSizeU0(),
                      patchMatrix[frameIdx][patchIdx].elem->getSizeV0() );

            std::cout << "Current Global Element" << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].weight =" << patchMatrix[frameIdx][patchIdx].weight << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].nextElemPos =" << patchMatrix[frameIdx][patchIdx].nextElemPos << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].prevElemPos =" << patchMatrix[frameIdx][patchIdx].prevElemPos << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].globalElem.getU1 =" << patchMatrix[frameIdx][patchIdx].globalElem.getU1() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].globalElem.getV1 =" << patchMatrix[frameIdx][patchIdx].globalElem.getV1() << std::endl;
            printMap( patchMatrix[frameIdx][patchIdx].globalElem.getOccupancy(),
                      patchMatrix[frameIdx][patchIdx].globalElem.getSizeU0(),
                      patchMatrix[frameIdx][patchIdx].globalElem.getSizeV0() );
          }

        } else {
          // first element in the chain, global parameters are equal to current element
          auto& curGlobalElem   = patchMatrix[frameIdx][patchIdx].globalElem;
          curGlobalElem.getU1() = patchMatrix[frameIdx][patchIdx].elem->getU1();
          curGlobalElem.getV1() = patchMatrix[frameIdx][patchIdx].elem->getV1();

          curGlobalElem.getSizeU0() = patchMatrix[frameIdx][patchIdx].elem->getSizeU0();
          curGlobalElem.getSizeV0() = patchMatrix[frameIdx][patchIdx].elem->getSizeV0();

          curGlobalElem.getOccupancy().resize( curGlobalElem.getSizeU0() * curGlobalElem.getSizeV0() );
          // copy the global occupancy map from current patch
          size_t curSizeU0 = patchMatrix[frameIdx][patchIdx].elem->getSizeU0();
          size_t curSizeV0 = patchMatrix[frameIdx][patchIdx].elem->getSizeV0();
          for ( size_t v = 0; v < curSizeV0; v++ ) {
            for ( size_t u = 0; u < curSizeU0; u++ ) {
              size_t posCur = u + curSizeU0 * ( v );
              patchMatrix[frameIdx][patchIdx].globalElem.getOccupancy()[posCur] =
                  patchMatrix[frameIdx][patchIdx].elem->getOccupancy()[posCur];
            }
          }
          curGlobalElem.getOccupancyResolution() = patchMatrix[frameIdx][patchIdx].elem->getOccupancyResolution();
          if ( printDetailedInfo ) {
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.ID =" << patchMatrix[frameIdx][patchIdx].elem->getIndex() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.U1 =" << patchMatrix[frameIdx][patchIdx].elem->getU1() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.V1 =" << patchMatrix[frameIdx][patchIdx].elem->getV1() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.sizeU0 =" << patchMatrix[frameIdx][patchIdx].elem->getSizeU0() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].elem.sizeV0 =" << patchMatrix[frameIdx][patchIdx].elem->getSizeV0() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].weight =" << patchMatrix[frameIdx][patchIdx].weight << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].nextElemPos =" << patchMatrix[frameIdx][patchIdx].nextElemPos << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].prevElemPos =" << patchMatrix[frameIdx][patchIdx].prevElemPos << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].globalElem.getU1 =" << patchMatrix[frameIdx][patchIdx].globalElem.getU1() << std::endl;
            std::cout << "patchMatrix[" << frameIdx << "][" << patchIdx
                      << "].globalElem.getV1 =" << patchMatrix[frameIdx][patchIdx].globalElem.getV1() << std::endl;
            printMap( patchMatrix[frameIdx][patchIdx].globalElem.getOccupancy(),
                      patchMatrix[frameIdx][patchIdx].globalElem.getSizeU0(),
                      patchMatrix[frameIdx][patchIdx].globalElem.getSizeV0() );
          }
        }
      }
    }
    // packing for each frame
    size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
    size_t occupancySizeV = ( std::max )( patchMatrix[0][0].elem->getSizeU0(), patchMatrix[0][0].elem->getSizeV0() );
    for ( size_t frameIdx = frStart; frameIdx < frEnd; frameIdx++ ) {
      auto& width  = frames[frameIdx].getWidth();
      auto& height = frames[frameIdx].getHeight();
      // sorting the list
      vector<PCCPatch> sortedPatches;
      auto&            patchUnsorted = patchMatrix[frameIdx];
      if ( frameIdx == frStart ) {
        sort( patchMatrixSortedIndexes[frameIdx].begin(), patchMatrixSortedIndexes[frameIdx].end(),
              [&patchUnsorted]( size_t i1, size_t i2 ) { return patchUnsorted[i1].gt( patchUnsorted[i2] ); } );
        if ( printDetailedInfo ) {
          for ( int patchIdx = 0; patchIdx < patchMatrixSortedIndexes[frameIdx].size(); patchIdx++ ) {
            auto& patch = patchMatrix[frameIdx][patchMatrixSortedIndexes[frameIdx][patchIdx]];
            std::cout << " New order Patch[" << patchIdx << "]->[" << patchMatrixSortedIndexes[frameIdx][patchIdx]
                      << "] (" << patchMatrix[frameIdx][patchMatrixSortedIndexes[frameIdx][patchIdx]].weight << "):";
            patch.elem->print();
          }
        }
      } else {
        // have to maintain the same order of the matched patches from previous frame
        // now sort the rest of the list according to the sort function of the class
        if ( frames[frameIdx].getNumMatchedPatches() != patchMatrixSortedIndexes[frameIdx].size() )
          sort( patchMatrixSortedIndexes[frameIdx].begin() + frames[frameIdx].getNumMatchedPatches(),
                patchMatrixSortedIndexes[frameIdx].end(),
                [&patchUnsorted]( size_t i1, size_t i2 ) { return patchUnsorted[i1].gt( patchUnsorted[i2] ); } );
        if ( printDetailedInfo ) {
          for ( int patchIdx = 0; patchIdx < frames[frameIdx].getNumMatchedPatches(); patchIdx++ ) {
            auto& patch = patchMatrix[frameIdx][patchMatrixSortedIndexes[frameIdx][patchIdx]];
            std::cout << " New order Matched (" << patch.prevElemPos << ") Patch[" << patchIdx << "]->["
                      << patchMatrixSortedIndexes[frameIdx][patchIdx] << "] (" << patch.weight << "):";
            patch.elem->print();
          }
          for ( int patchIdx = frames[frameIdx].getNumMatchedPatches();
                patchIdx < patchMatrixSortedIndexes[frameIdx].size(); patchIdx++ ) {
            auto& patch = patchMatrix[frameIdx][patchMatrixSortedIndexes[frameIdx][patchIdx]];
            std::cout << " New order Unmatched Patch[" << patchIdx << "]->["
                      << patchMatrixSortedIndexes[frameIdx][patchIdx] << "] (" << patch.weight << "):";
            patch.elem->print();
          }
        }
      }
      size_t maxOccupancyRow{0};

      vector<int>      orientation_vertical;
      vector<int>      orientation_horizontal;
      int              numOrientations;
      std::vector<int> horizon;
      if ( params_.packingStrategy_ == 0 ) {
        orientation_vertical.resize( 1 );
        orientation_vertical = {PATCH_ORIENTATION_DEFAULT};
        orientation_horizontal.resize( 1 );
        orientation_horizontal = {PATCH_ORIENTATION_DEFAULT};
        numOrientations        = 1;
      } else {
        if ( params_.packingStrategy_ == 1 ) {
          orientation_vertical.resize( 8 );
          orientation_vertical = {PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
                                  PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
                                  PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
          orientation_horizontal.resize( 8 );
          orientation_horizontal = {
              PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
              PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
              PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};  // favoring horizontal orientations (that should be
                                                                     // rotated)
          numOrientations = params_.useEightOrientations_ ? 8 : 2;
        } else {
          if ( params_.packingStrategy_ == 2 ) {
            orientation_vertical.resize( 8 );
            orientation_vertical = {PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
                                    PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
                                    PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};
            orientation_horizontal.resize( 8 );
            orientation_horizontal = {PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
                                      PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
                                      PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};
            numOrientations        = params_.useEightOrientations_ ? 8 : 2;
            horizon.resize( occupancySizeU, 0 );
          }
        }
      }
      std::vector<bool> occupancyMap;
      occupancyMap.resize( occupancySizeU * occupancySizeV, false );
      int indNextMatchedPatch = 0;
      // patch loop
      for ( int patchIdx = 0; patchIdx < patchMatrixSortedIndexes[frameIdx].size(); patchIdx++ ) {
        if ( printDetailedInfo ) {
          std::cout << "Processing patchMatrix[" << frameIdx << "][" << patchMatrixSortedIndexes[frameIdx][patchIdx]
                    << "]" << std::endl;
        }
        auto& curPatchElem  = patchMatrix[frameIdx][patchMatrixSortedIndexes[frameIdx][patchIdx]];
        auto& curGlobalElem = curPatchElem.globalElem;
        assert( curPatchElem.elem->getSizeU0() <= occupancySizeU );
        assert( curPatchElem.elem->getSizeV0() <= occupancySizeV );
        bool  locationFound = false;
        auto& occupancy     = curGlobalElem.getOccupancy();

        std::vector<int> top_horizon;
        std::vector<int> bottom_horizon;
        std::vector<int> right_horizon;
        std::vector<int> left_horizon;
        if ( params_.packingStrategy_ == 2 ) {
          // getting the horizons using the rotation 0 position
          curGlobalElem.get_patch_horizons( top_horizon, bottom_horizon, right_horizon, left_horizon );
        }

        while ( !locationFound ) {
          int    best_wasted_space = ( std::numeric_limits<int>::max )();
          size_t best_u, best_v;
          int    best_orientation;
          if ( curPatchElem.prevElemPos != InvalidPatchIndex ) {
            // try to place on the same position as the matched global patch, with the same orientation
            auto& previousGlobalElem            = patchMatrix[frameIdx - 1][curPatchElem.prevElemPos].globalElem;
            curGlobalElem.getPatchOrientation() = previousGlobalElem.getPatchOrientation();
            if ( !( curGlobalElem.isPatchDimensionSwitched() ) ) {
              curGlobalElem.getU0() =
                  previousGlobalElem.getU0() +
                  ( curGlobalElem.getU1() - previousGlobalElem.getU1() ) / curPatchElem.elem->getOccupancyResolution();
              curGlobalElem.getV0() =
                  previousGlobalElem.getV0() +
                  ( curGlobalElem.getV1() - previousGlobalElem.getV1() ) / curPatchElem.elem->getOccupancyResolution();
            } else {
              curGlobalElem.getU0() =
                  previousGlobalElem.getU0() +
                  ( curGlobalElem.getV1() - previousGlobalElem.getV1() ) / curPatchElem.elem->getOccupancyResolution();
              curGlobalElem.getV0() =
                  previousGlobalElem.getV0() +
                  ( curGlobalElem.getU1() - previousGlobalElem.getU1() ) / curPatchElem.elem->getOccupancyResolution();
            }
            if ( curGlobalElem.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                    params_.lowDelayEncoding_ ) ) {
              locationFound = true;
              if ( params_.packingStrategy_ == 2 ) {
                // saving the best position for tetris packing
                best_u           = curGlobalElem.getU0();
                best_v           = curGlobalElem.getV0();
                best_orientation = curGlobalElem.getPatchOrientation();
              }
              // now put the local patch in the relative position
              if ( !( curGlobalElem.isPatchDimensionSwitched() ) ) {
                curPatchElem.elem->getU0() =
                    curGlobalElem.getU0() + ( curPatchElem.elem->getU1() - curGlobalElem.getU1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
                curPatchElem.elem->getV0() =
                    curGlobalElem.getV0() + ( curPatchElem.elem->getV1() - curGlobalElem.getV1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
              } else {
                curPatchElem.elem->getU0() =
                    curGlobalElem.getU0() + ( curPatchElem.elem->getV1() - curGlobalElem.getV1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
                curPatchElem.elem->getV0() =
                    curGlobalElem.getV0() + ( curPatchElem.elem->getU1() - curGlobalElem.getU1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
              }
              curPatchElem.elem->getPatchOrientation() = curGlobalElem.getPatchOrientation();
              curPatchElem.elem->setBestMatchIdx( std::distance(
                  patchMatrixSortedIndexes[frameIdx - 1].begin(),
                  std::find( patchMatrixSortedIndexes[frameIdx - 1].begin(),
                             patchMatrixSortedIndexes[frameIdx - 1].end(), curPatchElem.prevElemPos ) ) );
              if ( params_.packingStrategy_ != 2 ) {
                // if it is not tetris packing, we can save the position in the list
                sortedPatches.push_back( *curPatchElem.elem );
                if ( printDetailedInfo ) {
                  std::cout << "Patch[" << curPatchElem.elem->getIndex() << "] maintained orientation "
                            << curPatchElem.elem->getPatchOrientation() << " for matched patch["
                            << curPatchElem.elem->getBestMatchIdx() << "] in the position ("
                            << curPatchElem.elem->getU0() << "," << curPatchElem.elem->getV0() << ")" << std::endl;
                }
                // if the element is the prediction of a patch in the next frame, store the current order for the next
                // frame, to maintain the sequence
                if ( curPatchElem.nextElemPos >= 0 )
                  patchMatrixSortedIndexes[frameIdx + 1][indNextMatchedPatch++] = curPatchElem.nextElemPos;
              }
            } else {
              std::cout << "Could not fit the global patch in the canvas, at position (" << curGlobalElem.getU0() << ","
                        << curGlobalElem.getV0() << ") and orientation " << curGlobalElem.getPatchOrientation()
                        << " something went wrong " << std::endl;
              if ( printDetailedInfo ) {
                printMap( occupancyMap, occupancySizeU, occupancySizeV );
                printMap( occupancy, curGlobalElem.getSizeU0(), curGlobalElem.getSizeV0() );
              }
            }
          } else {
            if ( curPatchElem.elem->getBestMatchIdx() != InvalidPatchIndex ) {
              // this is a matched patch, but the first element of the list,
              if ( printDetailedInfo ) {
                std::cout << "Reference moved from position prevList[" << curPatchElem.elem->getBestMatchIdx() << "] ";
              }
              int32_t matchedIdx = std::distance(
                  patchMatrixSortedIndexes[frameIdx - 1].begin(),
                  std::find( patchMatrixSortedIndexes[frameIdx - 1].begin(),
                             patchMatrixSortedIndexes[frameIdx - 1].end(), curPatchElem.elem->getBestMatchIdx() ) );
              if ( printDetailedInfo ) {
                std::cout << "to position sortedPrevList[" << matchedIdx
                          << "]=" << patchMatrix[frameIdx - 1][matchedIdx].elem->getIndex() << std::endl;
              }
              // the orientation MUST be maintained,  but it can be placed anywhere
              curGlobalElem.getPatchOrientation() = patchMatrix[frameIdx - 1][matchedIdx].elem->getPatchOrientation();
            }
            // best effort
            for ( size_t v = 0; v < occupancySizeV && ( ( params_.packingStrategy_ == 2 ) || !locationFound ); ++v ) {
              for ( size_t u = 0; u < occupancySizeU && ( ( params_.packingStrategy_ == 2 ) || !locationFound ); ++u ) {
                curGlobalElem.getU0() = u;
                curGlobalElem.getV0() = v;
                for ( size_t orientationIdx = 0;
                      orientationIdx < numOrientations && ( ( params_.packingStrategy_ == 2 ) || !locationFound );
                      orientationIdx++ ) {
                  if ( curPatchElem.elem->getBestMatchIdx() == InvalidPatchIndex ) {
                    if ( curGlobalElem.getSizeU0() > curGlobalElem.getSizeV0() ) {
                      curGlobalElem.getPatchOrientation() = orientation_horizontal[orientationIdx];
                    } else {
                      curGlobalElem.getPatchOrientation() = orientation_vertical[orientationIdx];
                    }
                  }
                  if ( params_.packingStrategy_ == 2 ) {
                    if ( !curGlobalElem.isPatchLocationAboveHorizon( horizon, top_horizon, bottom_horizon,
                                                                     right_horizon, left_horizon ) ) {
                      continue;
                    }
                  }
                  if ( curGlobalElem.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                          params_.lowDelayEncoding_ ) ) {
                    if ( params_.packingStrategy_ == 2 ) {
                      // now calculate the wasted space
                      int wasted_space = curGlobalElem.calculate_wasted_space( horizon, top_horizon, bottom_horizon,
                                                                               right_horizon, left_horizon );
                      if ( wasted_space < best_wasted_space ) {
                        best_wasted_space = wasted_space;
                        best_u            = u;
                        best_v            = v;
                        best_orientation  = curGlobalElem.getPatchOrientation();
                        locationFound     = true;
                      }
                    } else {
                      locationFound = true;
                      if ( !( curGlobalElem.isPatchDimensionSwitched() ) ) {
                        curPatchElem.elem->getU0() =
                            curGlobalElem.getU0() + ( curPatchElem.elem->getU1() - curGlobalElem.getU1() ) /
                                                        curPatchElem.elem->getOccupancyResolution();
                        curPatchElem.elem->getV0() =
                            curGlobalElem.getV0() + ( curPatchElem.elem->getV1() - curGlobalElem.getV1() ) /
                                                        curPatchElem.elem->getOccupancyResolution();
                      } else {
                        curPatchElem.elem->getU0() =
                            curGlobalElem.getU0() + ( curPatchElem.elem->getV1() - curGlobalElem.getV1() ) /
                                                        curPatchElem.elem->getOccupancyResolution();
                        curPatchElem.elem->getV0() =
                            curGlobalElem.getV0() + ( curPatchElem.elem->getU1() - curGlobalElem.getU1() ) /
                                                        curPatchElem.elem->getOccupancyResolution();
                      }
                      curPatchElem.elem->getPatchOrientation() = curGlobalElem.getPatchOrientation();
                      if ( curPatchElem.elem->getBestMatchIdx() >= 0 )
                        curPatchElem.elem->setBestMatchIdx(
                            std::distance( patchMatrixSortedIndexes[frameIdx - 1].begin(),
                                           std::find( patchMatrixSortedIndexes[frameIdx - 1].begin(),
                                                      patchMatrixSortedIndexes[frameIdx - 1].end(),
                                                      curPatchElem.elem->getBestMatchIdx() ) ) );
                      sortedPatches.push_back( *curPatchElem.elem );
                      if ( printDetailedInfo ) {
                        if ( curPatchElem.elem->getBestMatchIdx() >= 0 )
                          std::cout << "Orientation " << curPatchElem.elem->getPatchOrientation()
                                    << " maintained for matched patch " << curPatchElem.elem->getIndex() << " ("
                                    << curPatchElem.elem->getU0() << "," << curPatchElem.elem->getV0()
                                    << ") -> matchedPatch[" << curPatchElem.elem->getBestMatchIdx() << "]="
                                    << patchMatrix[frameIdx - 1][curPatchElem.elem->getBestMatchIdx()].elem->getIndex()
                                    << std::endl;
                        else
                          std::cout << "Orientation " << curPatchElem.elem->getPatchOrientation()
                                    << " selected for unmatched patch " << curPatchElem.elem->getIndex() << " ("
                                    << curPatchElem.elem->getU0() << "," << curPatchElem.elem->getV0() << ")"
                                    << std::endl;
                      }
                      // if the element is the prediction of a patch in the next frame, store the current order for the
                      // next frame, to maintain the sequence
                      if ( curPatchElem.nextElemPos >= 0 ) {
                        if ( printDetailedInfo ) {
                          std::cout << "Changing patchMatrixSortedIndexes[" << frameIdx + 1 << "]["
                                    << curPatchElem.nextElemPos << "] ("
                                    << patchMatrixSortedIndexes[frameIdx + 1][curPatchElem.nextElemPos] << ") ->";
                        }
                        patchMatrixSortedIndexes[frameIdx + 1][indNextMatchedPatch++] = curPatchElem.nextElemPos;
                        if ( printDetailedInfo ) {
                          std::cout << "(" << patchMatrixSortedIndexes[frameIdx + 1][curPatchElem.nextElemPos] << ")"
                                    << std::endl;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
          if ( !locationFound ) {
            occupancySizeV *= 2;
            occupancyMap.resize( occupancySizeU * occupancySizeV );
            if ( printDetailedInfo ) {
              std::cout << "Increasing the canvas size (" << occupancySizeU << "," << occupancySizeV << ")"
                        << std::endl;
            }
          } else {
            if ( params_.packingStrategy_ == 2 ) {
              curGlobalElem.getU0()               = best_u;
              curGlobalElem.getV0()               = best_v;
              curGlobalElem.getPatchOrientation() = best_orientation;
              if ( printDetailedInfo )
                std::cout << "Selected position (" << best_u << "," << best_v << ") and orientation "
                          << best_orientation << std::endl;
              if ( !( curGlobalElem.isPatchDimensionSwitched() ) ) {
                curPatchElem.elem->getU0() =
                    curGlobalElem.getU0() + ( curPatchElem.elem->getU1() - curGlobalElem.getU1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
                curPatchElem.elem->getV0() =
                    curGlobalElem.getV0() + ( curPatchElem.elem->getV1() - curGlobalElem.getV1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
              } else {
                curPatchElem.elem->getU0() =
                    curGlobalElem.getU0() + ( curPatchElem.elem->getV1() - curGlobalElem.getV1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
                curPatchElem.elem->getV0() =
                    curGlobalElem.getV0() + ( curPatchElem.elem->getU1() - curGlobalElem.getU1() ) /
                                                curPatchElem.elem->getOccupancyResolution();
              }
              curPatchElem.elem->getPatchOrientation() = curGlobalElem.getPatchOrientation();
              sortedPatches.push_back( *curPatchElem.elem );
              if ( printDetailedInfo ) {
                std::cout << "Orientation " << curPatchElem.elem->getPatchOrientation()
                          << " selected for unmatched patch " << curPatchElem.elem->getIndex() << " ("
                          << curPatchElem.elem->getU0() << "," << curPatchElem.elem->getV0() << ")" << std::endl;
              }
              // if the element is the prediction of a patch in the next frame, store the current order for the next
              // frame, to maintain the sequence
              if ( curPatchElem.nextElemPos >= 0 ) {
                if ( printDetailedInfo ) {
                  std::cout << "Changing patchMatrixSortedIndexes[" << frameIdx + 1 << "][" << curPatchElem.nextElemPos
                            << "] (" << patchMatrixSortedIndexes[frameIdx + 1][curPatchElem.nextElemPos] << ") ->";
                }
                patchMatrixSortedIndexes[frameIdx + 1][indNextMatchedPatch++] = curPatchElem.nextElemPos;
                if ( printDetailedInfo ) {
                  std::cout << "(" << patchMatrixSortedIndexes[frameIdx + 1][curPatchElem.nextElemPos] << ")"
                            << std::endl;
                }
              }
              // update the horizon
              curGlobalElem.update_horizon( horizon, top_horizon, bottom_horizon, right_horizon, left_horizon );
              // debugging
              if ( printDetailedInfo ) {
                std::cout << "New Horizon :[";
                for ( int i = 0; i < occupancySizeU; i++ ) { std::cout << horizon[i] << ","; }
                std::cout << "]" << std::endl;
              }
            }
          }
        }
        for ( size_t v0 = 0; v0 < curGlobalElem.getSizeV0(); ++v0 ) {
          for ( size_t u0 = 0; u0 < curGlobalElem.getSizeU0(); ++u0 ) {
            int coord = curGlobalElem.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
            if ( params_.lowDelayEncoding_ )
              occupancyMap[coord] = true;
            else
              occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * curGlobalElem.getSizeU0() + u0];
          }
        }
        if ( !( curGlobalElem.isPatchDimensionSwitched() ) ) {
          height = ( std::max )(
              height, ( curGlobalElem.getV0() + curGlobalElem.getSizeV0() ) * curGlobalElem.getOccupancyResolution() );
          width = ( std::max )(
              width, ( curGlobalElem.getU0() + curGlobalElem.getSizeU0() ) * curGlobalElem.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGlobalElem.getV0() + curGlobalElem.getSizeV0() ) );
        } else {
          height = ( std::max )(
              height, ( curGlobalElem.getV0() + curGlobalElem.getSizeU0() ) * curGlobalElem.getOccupancyResolution() );
          width = ( std::max )(
              width, ( curGlobalElem.getU0() + curGlobalElem.getSizeV0() ) * curGlobalElem.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGlobalElem.getV0() + curGlobalElem.getSizeU0() ) );
        }

        if ( printDetailedInfo ) {
          if ( params_.packingStrategy_ != 2 ) {
            printMap( occupancyMap, occupancySizeU, occupancySizeV );
          } else {
            printMapTetris( occupancyMap, occupancySizeU, occupancySizeV, horizon );
          }
        }
      }
      // update the sorted list of patches
      frames[frameIdx].getPatches() = sortedPatches;

      if ( frames[frameIdx].getNumberOfMissedPointsPatches() > 0 &&
           !frames[frameIdx].getUseMissedPointsSeparateVideo() ) {
        packMissedPointsPatch( frames[frameIdx], occupancyMap, width, height, occupancySizeU, occupancySizeV,
                               maxOccupancyRow );
      }
      if ( params_.enhancedDeltaDepthCode_ && !frames[frameIdx].getUseMissedPointsSeparateVideo() ) {
        packEOMTexturePointsPatch( frames[frameIdx], occupancyMap, width, height, occupancySizeU, occupancySizeV,
                                   maxOccupancyRow );
      }
      if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
      std::cout << "actualImageSize " << width << " x " << height << std::endl;
    }
  }
}

void PCCEncoder::pack( PCCFrameContext& frame, int safeguard, bool enablePointCloudPartitioning ) {
  auto& width   = frame.getWidth();
  auto& height  = frame.getHeight();
  auto& patches = frame.getPatches();
  if ( patches.empty() ) { return; }
  std::sort( patches.begin(), patches.end() );
  size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t occupancySizeV =
      ( std::max )( params_.minimumImageHeight_ / params_.occupancyResolution_, patches[0].getSizeV0() );
  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }

  int numROIs     = params_.numROIs_;
  int numTilesHor = params_.numTilesHor_;
  int tileWidth   = occupancySizeU / numTilesHor;
  int tileHeight  = int( tileWidth * params_.tileHeightToWidthRatio_ );
  std::cout << "tile size  = " << tileWidth << " x " << tileHeight << std::endl;
  occupancySizeV = ( occupancySizeV >= tileHeight ) ? occupancySizeV : tileHeight;

  width  = occupancySizeU * params_.occupancyResolution_;
  height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  std::vector<bool> occupancyMap;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  if ( !enablePointCloudPartitioning ) {
    for ( auto& patch : patches ) {
      assert( patch.getSizeU0() <= occupancySizeU );
      assert( patch.getSizeV0() <= occupancySizeV );
      bool  locationFound = false;
      auto& occupancy     = patch.getOccupancy();
      while ( !locationFound ) {
        patch.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;  // only allowed orientation in anchor
        for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
          for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
            patch.getU0() = u;
            patch.getV0() = v;
            if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                            safeguard ) ) {
              locationFound = true;
            }
          }
        }
        if ( !locationFound ) {
          occupancySizeV *= 2;
          occupancyMap.resize( occupancySizeU * occupancySizeV );
        }
      }
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
        const size_t v = patch.getV0() + v0;
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
          const size_t u = patch.getU0() + u0;
          if ( params_.lowDelayEncoding_ )
            occupancyMap[v * occupancySizeU + u] = true;
          else
            occupancyMap[v * occupancySizeU + u] =
                occupancyMap[v * occupancySizeU + u] || occupancy[v0 * patch.getSizeU0() + u0];
        }
      }

      height          = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      width           = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
      // print(occupancyMap, occupancySizeU, occupancySizeV);
    }
  } else {
    std::vector<Tile> tilesNotAvailable;  // set of all tiles occupied by prev ROIs of current ROI
    int               lastOccupiedTileIndex          = -1;
    int               lastOccupiedTileIndexByPrevROI = -1;
    // loop over ROIs
    for ( int roiIndex = 0; roiIndex < numROIs; ++roiIndex ) {
      tilesNotAvailable.clear();
      if ( roiIndex > 0 ) {
        lastOccupiedTileIndexByPrevROI = lastOccupiedTileIndex;
        for ( int tileIndex = 0; tileIndex <= lastOccupiedTileIndexByPrevROI; ++tileIndex ) {
          Tile tile;
          tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
          tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
          tile.maxU = tile.minU + tileWidth - 1;
          tile.maxV = tile.minV + tileHeight - 1;
          tilesNotAvailable.push_back( tile );
        }
      }
      // loop over patches of current ROI
      for ( auto& patch : patches ) {
        if ( roiIndex != patch.getRoiIndex() ) { continue; }
        assert( patch.getSizeU0() <= occupancySizeU );
        assert( patch.getSizeV0() <= occupancySizeV );
        bool  locationFound = false;
        auto& occupancy     = patch.getOccupancy();
        while ( !locationFound ) {
          patch.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;  // only allowed orientation in anchor
          int numTilesAvailable       = ceil( double( occupancySizeV ) / double( tileHeight ) ) * numTilesHor;
          for ( int tileIndex = lastOccupiedTileIndexByPrevROI + 1; tileIndex < numTilesAvailable && !locationFound;
                ++tileIndex ) {
            Tile tile;
            tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
            tile.maxU = tile.minU + tileWidth - 1;
            tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
            tile.maxV = tile.minV + tileHeight - 1;
            for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
              for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
                patch.getU0()        = u;
                patch.getV0()        = v;
                bool tileIsAvailable = true;
                if ( roiIndex > 0 ) {
                  for ( auto& tile : tilesNotAvailable ) {
                    if ( ( tile.minU <= u && u <= tile.maxU ) && ( tile.minV <= v && v <= tile.maxV ) &&
                         ( !locationFound ) ) {
                      tileIsAvailable = false;
                      break;
                    }
                  }
                }
                if ( tileIsAvailable ) {
                  if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                  params_.lowDelayEncoding_, safeguard, tile ) ) {
                    locationFound = true;
                    if ( tileIndex > lastOccupiedTileIndex ) { lastOccupiedTileIndex = tileIndex; }
                    std::cout << "ROI-" << roiIndex + 1 << " patch-" << patch.getIndex() << " fitted in tile-"
                              << tileIndex + 1 << "/" << numTilesAvailable << "-----[" << tile.minU << "," << tile.maxU
                              << "][" << tile.minV << "," << tile.maxV << "]" << std::endl;
                  }
                }
              }
            }
          }
          if ( !locationFound ) {
            occupancySizeV *= 2;
            occupancyMap.resize( occupancySizeU * occupancySizeV );
          }
        }
        for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
          const size_t v = patch.getV0() + v0;
          for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
            const size_t u = patch.getU0() + u0;
            occupancyMap[v * occupancySizeU + u] =
                occupancyMap[v * occupancySizeU + u] || occupancy[v0 * patch.getSizeU0() + u0];
          }
        }

        height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
        width  = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
        // print(occupancyMap, occupancySizeU, occupancySizeV);
      }  // patch loop
      printMap( occupancyMap, occupancySizeU, occupancySizeV );
    }  // ROI loop
  }

  if ( frame.getNumberOfMissedPointsPatches() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
    packMissedPointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  } else {
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  }
  if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
    packEOMTexturePointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  }
  if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  std::cout << "actualImageSize " << width << " x " << height << std::endl;
}

void PCCEncoder::packFlexible( PCCFrameContext& frame, int safeguard, bool enablePointCloudPartitioning ) {
  auto& width   = frame.getWidth();
  auto& height  = frame.getHeight();
  auto& patches = frame.getPatches();
  // set no matched patches, since this function does not take into account the previous frame
  frame.setNumMatchedPatches( 0 );
  if ( patches.empty() ) { return; }
  // sorting by patch largest dimension
  std::sort( patches.begin(), patches.end(), []( PCCPatch& a, PCCPatch& b ) { return a.gt( b ); } );
  if ( printDetailedInfo ) {
    std::cout << "Patch order:" << std::endl;
    for ( auto& patch : patches ) {
      std::cout << "Patch[" << patch.getIndex() << "]=(" << patch.getSizeU0() << "," << patch.getSizeV0() << ")"
                << std::endl;
    }
  }

  size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t occupancySizeV = ( std::max )( patches[0].getSizeV0(), patches[0].getSizeU0() );
  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }

  int numROIs     = params_.numROIs_;
  int numTilesHor = params_.numTilesHor_;
  int tileWidth   = occupancySizeU / numTilesHor;
  int tileHeight  = int( tileWidth * params_.tileHeightToWidthRatio_ );
  std::cout << "tile size  = " << tileWidth << " x " << tileHeight << std::endl;
  occupancySizeV = ( occupancySizeV >= tileHeight ) ? occupancySizeV : tileHeight;

  width  = occupancySizeU * params_.occupancyResolution_;
  height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  std::vector<bool> occupancyMap;
  vector<int>       orientation_vertical = {
      PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
  vector<int> orientation_horizontal = {
      PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};  // favoring horizontal orientations (that should be
                                                             // rotated)
  int numOrientations = params_.useEightOrientations_ ? 8 : 2;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  if ( !params_.enablePointCloudPartitioning_ ) {
    for ( auto& patch : patches ) {
      assert( patch.getSizeU0() <= occupancySizeU );
      assert( patch.getSizeV0() <= occupancySizeV );
      bool  locationFound = false;
      auto& occupancy     = patch.getOccupancy();
      while ( !locationFound ) {
        for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
          for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
            patch.getU0() = u;
            patch.getV0() = v;
            for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound; orientationIdx++ ) {
              if ( patch.getSizeU0() > patch.getSizeV0() ) {
                patch.getPatchOrientation() = orientation_horizontal[orientationIdx];
              } else {
                patch.getPatchOrientation() = orientation_vertical[orientationIdx];
              }
              if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                              safeguard ) ) {
                locationFound = true;
                if ( printDetailedInfo ) {
                  std::cout << "Orientation " << patch.getPatchOrientation() << " selected for patch "
                            << patch.getIndex() << " (" << u << "," << v << ")" << std::endl;
                }
              }
            }
          }
        }
        if ( !locationFound ) {
          occupancySizeV *= 2;
          occupancyMap.resize( occupancySizeU * occupancySizeV );
        }
      }
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
          int coord = patch.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
          if ( params_.lowDelayEncoding_ )
            occupancyMap[coord] = true;
          else
            occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
        }
      }

      if ( !( patch.isPatchDimensionSwitched() ) ) {
        height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
        width  = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
      } else {
        height = ( std::max )( height, ( patch.getV0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
        width  = ( std::max )( width, ( patch.getU0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeU0() ) );
      }
    }
  } else {
    std::vector<Tile> tilesNotAvailable;  // set of all tiles occupied by prev ROIs of current ROI
    int               lastOccupiedTileIndex          = -1;
    int               lastOccupiedTileIndexByPrevROI = -1;
    // loop over ROIs
    for ( int roiIndex = 0; roiIndex < numROIs; ++roiIndex ) {
      tilesNotAvailable.clear();
      if ( roiIndex > 0 ) {
        lastOccupiedTileIndexByPrevROI = lastOccupiedTileIndex;
        for ( int tileIndex = 0; tileIndex <= lastOccupiedTileIndexByPrevROI; ++tileIndex ) {
          Tile tile;
          tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
          tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
          tile.maxU = tile.minU + tileWidth - 1;
          tile.maxV = tile.minV + tileHeight - 1;
          tilesNotAvailable.push_back( tile );
        }
      }
      // loop over patches of current ROI
      for ( auto& patch : patches ) {
        if ( roiIndex != patch.getRoiIndex() ) { continue; }
        assert( patch.getSizeU0() <= occupancySizeU );
        assert( patch.getSizeV0() <= occupancySizeV );
        bool  locationFound = false;
        auto& occupancy     = patch.getOccupancy();
        // fit patch in available tiles (i.e., tiles not occupied by previous ROIs)
        while ( !locationFound ) {
          int numTilesAvailable = ceil( double( occupancySizeV ) / double( tileHeight ) ) * numTilesHor;
          for ( int tileIndex = lastOccupiedTileIndexByPrevROI + 1; tileIndex < numTilesAvailable && !locationFound;
                ++tileIndex ) {
            Tile tile;
            tile.minU = ( tileIndex % numTilesHor ) * tileWidth;
            tile.maxU = tile.minU + tileWidth - 1;
            tile.minV = ( tileIndex / numTilesHor ) * tileHeight;
            tile.maxV = tile.minV + tileHeight - 1;
            for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
              for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
                patch.getU0()        = u;
                patch.getV0()        = v;
                bool tileIsAvailable = true;
                if ( roiIndex > 0 ) {
                  for ( auto& tile : tilesNotAvailable ) {
                    if ( ( tile.minU <= u && u <= tile.maxU ) && ( tile.minV <= v && v <= tile.maxV ) &&
                         ( !locationFound ) ) {
                      tileIsAvailable = false;
                      break;
                    }
                  }
                }
                if ( tileIsAvailable ) {
                  for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound;
                        orientationIdx++ ) {
                    if ( patch.getSizeU0() > patch.getSizeV0() ) {
                      patch.getPatchOrientation() = orientation_horizontal[orientationIdx];
                    } else {
                      patch.getPatchOrientation() = orientation_vertical[orientationIdx];
                    }
                    if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                    params_.lowDelayEncoding_, safeguard, tile ) ) {
                      locationFound = true;
                      if ( tileIndex > lastOccupiedTileIndex ) { lastOccupiedTileIndex = tileIndex; }
                      std::cout << "ROI-" << roiIndex + 1 << " patch-" << patch.getIndex() << " fitted in tile-"
                                << tileIndex + 1 << "/" << numTilesAvailable << "-----[" << tile.minU << ","
                                << tile.maxU << "][" << tile.minV << "," << tile.maxV << "]" << std::endl;
                      if ( printDetailedInfo ) {
                        std::cout << "Orientation " << patch.getPatchOrientation() << " selected for patch "
                                  << patch.getIndex() << " (" << u << "," << v << ")" << std::endl;
                      }
                    }
                  }
                }
              }
            }
          }
          if ( !locationFound ) {
            occupancySizeV *= 2;
            occupancyMap.resize( occupancySizeU * occupancySizeV );
          }
        }  // while loop
        for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
          for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
            int coord           = patch.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
            occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
          }
        }

        if ( !( patch.isPatchDimensionSwitched() ) ) {
          height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
          width  = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
        } else {
          height = ( std::max )( height, ( patch.getV0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
          width  = ( std::max )( width, ( patch.getU0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeU0() ) );
        }
      }  // patch loop
      printMap( occupancyMap, occupancySizeU, occupancySizeV );
    }  // ROI loop
  }

  if ( frame.getNumberOfMissedPointsPatches() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
    packMissedPointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  } else {
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  }
  if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
    packEOMTexturePointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  }
  if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  std::cout << "actualImageSize " << width << " x " << height << std::endl;
}

void PCCEncoder::packTetris( PCCFrameContext& frame, int safeguard ) {
  auto& width   = frame.getWidth();
  auto& height  = frame.getHeight();
  auto& patches = frame.getPatches();
  // set no matched patches, since this function does not take into account the previous frame
  frame.setNumMatchedPatches( 0 );
  if ( patches.empty() ) { return; }
  // sorting by patch largest dimension
  std::sort( patches.begin(), patches.end(), []( PCCPatch& a, PCCPatch& b ) { return a.gt( b ); } );
  if ( printDetailedInfo ) {
    std::cout << "Patch order:" << std::endl;
    for ( auto& patch : patches ) {
      std::cout << "Patch[" << patch.getIndex() << "]=(" << patch.getSizeU0() << "," << patch.getSizeV0() << ")"
                << std::endl;
    }
  }
  size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t occupancySizeV = ( std::max )( patches[0].getSizeV0(), patches[0].getSizeU0() );
  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }

  width  = occupancySizeU * params_.occupancyResolution_;
  height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  std::vector<bool> occupancyMap;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  std::vector<int> horizon;
  horizon.resize( occupancySizeU, 0 );
  if ( printDetailedInfo ) {
    std::cout << "Horizon :[";
    for ( int i = 0; i < occupancySizeU; i++ ) { std::cout << horizon[i] << ","; }
    std::cout << "]" << std::endl;
  }

  for ( auto& patch : patches ) {
    assert( patch.getSizeU0() <= occupancySizeU );
    assert( patch.getSizeV0() <= occupancySizeV );
    auto& occupancy = patch.getOccupancy();

    // getting the horizons using the rotation 0 position
    if ( printDetailedInfo ) patch.print();
    std::vector<int> top_horizon;
    std::vector<int> bottom_horizon;
    std::vector<int> right_horizon;
    std::vector<int> left_horizon;
    patch.get_patch_horizons( top_horizon, bottom_horizon, right_horizon, left_horizon );

    bool locationFound = false;
    // try to place the patch tetris-style
    vector<int> orientation_values = {
        PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
        PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
        PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
    int numOrientations = params_.useEightOrientations_ ? 8 : 2;
    while ( !locationFound ) {
      int    best_wasted_space = ( std::numeric_limits<int>::max )();
      size_t best_u, best_v;
      int    best_orientation;
      for ( size_t u = 0; u < occupancySizeU; ++u ) {
        for ( size_t v = 0; v < occupancySizeV; ++v ) {
          patch.getU0() = u;
          patch.getV0() = v;
          for ( size_t orientationIdx = 0; orientationIdx < numOrientations; orientationIdx++ ) {
            patch.getPatchOrientation() = orientation_values[orientationIdx];
            if ( !patch.isPatchLocationAboveHorizon( horizon, top_horizon, bottom_horizon, right_horizon,
                                                     left_horizon ) ) {
              if ( printDetailedInfo )
                std::cout << "(" << u << "," << v << "|" << patch.getPatchOrientation() << ") above horizon"
                          << std::endl;
              continue;
            }
            if ( printDetailedInfo )
              std::cout << "(" << u << "," << v << "|" << patch.getPatchOrientation() << ")" << std::endl;
            if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                            safeguard ) ) {
              // now calculate the wasted space
              int wasted_space =
                  patch.calculate_wasted_space( horizon, top_horizon, bottom_horizon, right_horizon, left_horizon );
              if ( printDetailedInfo ) std::cout << "(wasted space) = " << wasted_space << std::endl;
              if ( wasted_space < best_wasted_space ) {
                best_wasted_space = wasted_space;
                best_u            = u;
                best_v            = v;
                best_orientation  = patch.getPatchOrientation();
                locationFound     = true;
              }
            }
          }
        }
      }
      if ( !locationFound ) {
        occupancySizeV *= 2;
        occupancyMap.resize( occupancySizeU * occupancySizeV );
        if ( printDetailedInfo )
          std::cout << "Increasing frame size (" << occupancySizeU << "," << occupancySizeV << ")" << std::endl;
      } else {
        // select the best position and orientation
        patch.getU0()               = best_u;
        patch.getV0()               = best_v;
        patch.getPatchOrientation() = best_orientation;
        if ( printDetailedInfo )
          std::cout << "Selected position (" << best_u << "," << best_v << ") and orientation " << best_orientation
                    << "(wasted space=" << best_wasted_space << ")" << std::endl;
        // update the horizon
        patch.update_horizon( horizon, top_horizon, bottom_horizon, right_horizon, left_horizon );
        // debugging
        if ( printDetailedInfo ) {
          std::cout << "Horizon :[";
          for ( int i = 0; i < occupancySizeU; i++ ) { std::cout << horizon[i] << ","; }
          std::cout << "]" << std::endl;
        }
      }
    }
    for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
      for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
        int coord = patch.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
        if ( params_.lowDelayEncoding_ )
          occupancyMap[coord] = true;
        else
          occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
      }
    }
    if ( !( patch.isPatchDimensionSwitched() ) ) {
      height          = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      width           = ( std::max )( width, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
    } else {
      height          = ( std::max )( height, ( patch.getV0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      width           = ( std::max )( width, ( patch.getU0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeU0() ) );
    }

    if ( printDetailedInfo ) { printMapTetris( occupancyMap, occupancySizeU, occupancySizeV, horizon ); }
  }

  if ( frame.getNumberOfMissedPointsPatches() > 0 ) {
    packMissedPointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  } else {
    if ( printDetailedInfo ) printMap( occupancyMap, occupancySizeU, occupancySizeV );
  }
  if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
    packEOMTexturePointsPatch( frame, occupancyMap, width, height, occupancySizeU, occupancySizeV, maxOccupancyRow );
  }
  if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
  std::cout << "actualImageSize " << width << " x " << height << std::endl;
}

void PCCEncoder::packEOMTexturePointsPatch( PCCFrameContext&   frame,
                                            std::vector<bool>& occupancyMap,
                                            size_t&            width,
                                            size_t&            height,
                                            size_t             occupancySizeU,
                                            size_t             occupancySizeV,
                                            size_t             maxOccupancyRow ) {
  assert( width == frame.getWidth() );
  assert( height == frame.getHeight() );
  auto&  eomPatches = frame.getEomPatches();
  size_t lastHeight = height;
  for ( size_t i = 0; i < eomPatches.size(); i++ ) {
    size_t eddPointsPatchBlocks = static_cast<size_t>(
        ceil( double( eomPatches[i].eddCount_ ) / ( params_.occupancyResolution_ * params_.occupancyResolution_ ) ) );
    size_t eddPointsPatchBlocksV = static_cast<size_t>( ceil( double( eddPointsPatchBlocks ) / occupancySizeU ) );
    occupancySizeV += eddPointsPatchBlocksV;
    eomPatches[i].u0_    = 0;
    eomPatches[i].v0_    = lastHeight / params_.occupancyResolution_;
    eomPatches[i].sizeU_ = occupancySizeU;
    eomPatches[i].sizeV_ = eddPointsPatchBlocksV;
#if 1
    printf( "packEOMTexturePointsPatch[%zu]: eomPatch[0]: %zu,%zu(block) (%zux%zu)(block), #EDDBlock:%zu #EDD:%zu\n", i,
            eomPatches[i].u0_, eomPatches[i].v0_, eomPatches[i].sizeU_, eomPatches[i].sizeV_, eddPointsPatchBlocks,
            eomPatches[i].eddCount_ );
#endif
    lastHeight += eomPatches[i].sizeV_ * params_.occupancyResolution_;
  }
  occupancyMap.resize( occupancySizeU * occupancySizeV );
  height = lastHeight;
}

void PCCEncoder::packMissedPointsPatch( PCCFrameContext&   frame,
                                        std::vector<bool>& occupancyMap,
                                        size_t&            width,
                                        size_t&            height,
                                        size_t             occupancySizeU,
                                        size_t             occupancySizeV,
                                        size_t             maxOccupancyRow ) {
  size_t numberOfMpsPatches = frame.getNumberOfMissedPointsPatches();
  size_t safeguard          = 0;
  for ( int i = 0; i < numberOfMpsPatches; i++ ) {
    auto&  missedPointsPatch       = frame.getMissedPointsPatch( i );
    size_t missedPointsPatchBlocks = static_cast<size_t>(
        ceil( double( missedPointsPatch.size() ) / ( params_.occupancyResolution_ * params_.occupancyResolution_ ) ) );
    size_t missedPointsPatchBlocksV0 =
        static_cast<size_t>( ceil( double( missedPointsPatchBlocks ) / occupancySizeU ) );
    size_t missedPointsPatchBlocksU0 =
        static_cast<size_t>( ceil( double( missedPointsPatchBlocks ) / missedPointsPatchBlocksV0 ) );
    missedPointsPatch.sizeU0_ = missedPointsPatchBlocksU0;
    missedPointsPatch.sizeV0_ = missedPointsPatchBlocksV0;
    missedPointsPatch.sizeV_  = missedPointsPatchBlocksV0 * params_.occupancyResolution_;
    missedPointsPatch.sizeU_  = missedPointsPatchBlocksU0 * params_.occupancyResolution_;
    PCCPatch patch;
    patch.getSizeU0() = missedPointsPatch.sizeU0_;
    patch.getSizeV0() = missedPointsPatch.sizeV0_;
    patch.getSizeU()  = missedPointsPatch.sizeU_;
    patch.getSizeV()  = missedPointsPatch.sizeV_;
    assert( patch.getSizeU0() <= occupancySizeU );
    // assert( patch.getSizeV0() <= occupancySizeV );
    std::vector<bool>& patchOccupancy = patch.getOccupancy();
    patchOccupancy.resize( missedPointsPatch.sizeU0_ * missedPointsPatch.sizeV0_, false );

    const int16_t infiniteValue = ( std::numeric_limits<int16_t>::max )();
    missedPointsPatch.resize( missedPointsPatch.sizeU_ * missedPointsPatch.sizeV_, infiniteValue );
    std::vector<bool>& missedPointPatchOccupancy = missedPointsPatch.occupancy_;
    missedPointPatchOccupancy.resize( missedPointsPatch.sizeU0_ * missedPointsPatch.sizeV0_, false );

    for ( size_t v = 0; v < missedPointsPatch.sizeV_; ++v ) {
      for ( size_t u = 0; u < missedPointsPatch.sizeU_; ++u ) {
        const size_t p = v * missedPointsPatch.sizeU_ + u;
        if ( missedPointsPatch.x_[p] < infiniteValue ) {
          const size_t u0 = u / missedPointsPatch.occupancyResolution_;
          const size_t v0 = v / missedPointsPatch.occupancyResolution_;
          const size_t p0 = v0 * missedPointsPatch.sizeU0_ + u0;
          assert( u0 >= 0 && u0 < missedPointsPatch.sizeU0_ );
          assert( v0 >= 0 && v0 < missedPointsPatch.sizeV0_ );
          missedPointPatchOccupancy[p0] = true;
          patchOccupancy[p0]            = true;
        }
      }
    }

    // now placing the missed point patch in the atlas
    bool locationFound = false;
    while ( !locationFound ) {
      patch.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;  // only allowed orientation in anchor
      for ( int v = maxOccupancyRow; v <= occupancySizeV && !locationFound; ++v ) {
        for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
          patch.getU0() = u;
          patch.getV0() = v;
          if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                          safeguard ) ) {
            locationFound = true;
          }
        }
      }
      if ( !locationFound ) {
        occupancySizeV *= 2;
        occupancyMap.resize( occupancySizeU * occupancySizeV );
      }
    }
    missedPointsPatch.u0_ = patch.getU0();
    missedPointsPatch.v0_ = patch.getV0();

    for ( size_t v0 = 0; v0 < missedPointsPatch.sizeV0_; ++v0 ) {
      const size_t v = missedPointsPatch.v0_ + v0;
      for ( size_t u0 = 0; u0 < missedPointsPatch.sizeU0_; ++u0 ) {
        const size_t u = missedPointsPatch.u0_ + u0;
        if ( params_.lowDelayEncoding_ )
          occupancyMap[v * occupancySizeU + u] = true;
        else
          occupancyMap[v * occupancySizeU + u] =
              occupancyMap[v * occupancySizeU + u] || missedPointPatchOccupancy[v0 * missedPointsPatch.sizeU0_ + u0];
      }
      height = ( std::max )( height, ( patch.getV0() + patch.getSizeV0() ) * params_.occupancyResolution_ );
    }
  }
}

#define MINIMUM_TH_WEIGHT 0.6

struct mypair {
  int      idx;
  uint32_t value;
};

bool comp1( const mypair& a, const mypair& b ) { return a.value < b.value; }

void PCCEncoder::calculateWeightNormal( PCCContext& context, const PCCPointSet3& source, PCCFrameContext& frame ) {
  size_t atlasIndex         = 0;
  auto&  gi                 = context.getSps().getGeometryInformation( atlasIndex );
  size_t geometryBitDepth3D = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
  size_t maxValue           = 1 << geometryBitDepth3D;

  PCCVector3D weight_value;
  bool*       pj_face    = new bool[maxValue * maxValue * 3];
  size_t      pointCount = source.getPointCount();

  if ( params_.enhancedPP_ ) {
    for ( size_t idx = 0; idx < maxValue * maxValue * 3; idx++ ) { *( pj_face + idx ) = false; }

    const int size_1f = maxValue * maxValue;
    for ( size_t idx = 0; idx < pointCount; idx++ ) {
      const PCCPoint3D point = source[idx];
      int              x, y;
      // YZ: 0,3
      x                               = int( point[1] );
      y                               = int( point[2] );
      *( pj_face + y * maxValue + x ) = true;

      // ZX: 0,3
      x                                         = int( point[2] );
      y                                         = int( point[0] );
      *( pj_face + y * maxValue + x + size_1f ) = true;

      // XY: 0,3
      x                                             = int( point[0] );
      y                                             = int( point[1] );
      *( pj_face + y * maxValue + x + size_1f * 2 ) = true;
    }

    mypair pj_cnt[3];
    for ( int x = 0; x < 3; x++ ) {
      pj_cnt[x].idx   = x;
      pj_cnt[x].value = 0;
    }
    for ( size_t idx = 0; idx < maxValue * maxValue; idx++ ) {
      if ( *( pj_face + idx ) == true ) { pj_cnt[0].value = pj_cnt[0].value + 1; }

      if ( *( pj_face + idx + size_1f ) == true ) { pj_cnt[1].value = pj_cnt[1].value + 1; }

      if ( *( pj_face + idx + size_1f * 2 ) == true ) { pj_cnt[2].value = pj_cnt[2].value + 1; }
    }

    std::sort( pj_cnt, pj_cnt + 3, comp1 );

    double axis_weight[6];
    if ( ( double( pj_cnt[0].value ) / double( pj_cnt[2].value ) ) >= params_.minWeightEPP_ ) {
      int idx_t          = pj_cnt[0].idx;
      axis_weight[idx_t] = axis_weight[idx_t + 3] = double( pj_cnt[0].value ) / double( pj_cnt[2].value );

      idx_t              = pj_cnt[1].idx;
      axis_weight[idx_t] = axis_weight[idx_t + 3] = double( pj_cnt[1].value ) / double( pj_cnt[2].value );

      idx_t              = pj_cnt[2].idx;
      axis_weight[idx_t] = axis_weight[idx_t + 3] = 1.0;
    } else {
      int    idx_t = pj_cnt[0].idx;
      double tmpb, tmpa;
      axis_weight[idx_t] = axis_weight[idx_t + 3] = params_.minWeightEPP_;

      idx_t              = pj_cnt[2].idx;
      axis_weight[idx_t] = axis_weight[idx_t + 3] = 1.0;

      idx_t = pj_cnt[1].idx;
      tmpb  = double( pj_cnt[1].value ) / double( pj_cnt[2].value );
      tmpa  = double( pj_cnt[0].value ) / double( pj_cnt[2].value );

      axis_weight[idx_t] = axis_weight[idx_t + 3] =
          params_.minWeightEPP_ + ( tmpb - tmpa ) / ( 1.0 - tmpa ) * ( 1 - params_.minWeightEPP_ );
    }

    weight_value[0] = axis_weight[0];
    weight_value[1] = axis_weight[1];
    weight_value[2] = axis_weight[2];
  } else {
    weight_value[0] = 1.0;
    weight_value[1] = 1.0;
    weight_value[2] = 1.0;
  }

  frame.getWeightNormal() = weight_value;
}

bool PCCEncoder::generateScaledGeometry( const PCCPointSet3& source, PCCFrameContext& frame ) {
  auto& patches = frame.getPatches();
  //  const int16_t infiniteDepth = ( std::numeric_limits<int16_t>::max )();
  std::sort( patches.begin(), patches.end() );
  for ( size_t i = 0; i < 3; i++ ) {
    std::vector<int16_t> depth[2];  // depth
    size_t               scaleSizeU = patches[i].getSizeU() / params_.levelOfDetailX_;
    size_t               scaleSizeV = patches[i].getSizeV() / params_.levelOfDetailY_;
    depth[0].resize( scaleSizeU * scaleSizeV );
    depth[1].resize( scaleSizeU * scaleSizeV );

    for ( size_t v = 0; v < scaleSizeV; v++ ) {
      for ( size_t u = 0; u < scaleSizeU; u++ ) {
        size_t p       = v * params_.levelOfDetailY_ * patches[i].getSizeU() + u * params_.levelOfDetailX_;
        size_t pScaled = v * scaleSizeU + u;

        if ( patches[i].getDepth( 0 )[p] == infiniteDepth ) {
          depth[0][pScaled] = depth[1][pScaled] = infiniteDepth;
        } else {
          depth[0][pScaled] = patches[i].getDepth( 0 )[p];
          depth[1][pScaled] = patches[i].getDepth( 1 )[p];
        }
      }
    }

    patches[i].setLodScaleX( params_.levelOfDetailX_ );
    patches[i].setLodScaleY( params_.levelOfDetailY_ );
    patches[i].getSizeU()  = scaleSizeU;
    patches[i].getSizeV()  = scaleSizeV;
    patches[i].getSizeU0() = std::ceil( (double)scaleSizeU / params_.occupancyResolution_ );
    patches[i].getSizeV0() = std::ceil( (double)scaleSizeV / params_.occupancyResolution_ );

    patches[i].getOccupancy().clear();
    patches[i].getDepth( 0 ).clear();
    patches[i].getDepth( 1 ).clear();
    patches[i].getOccupancy().resize( patches[i].getSizeU0() * patches[i].getSizeV0(), false );
    patches[i].getDepth( 0 ).resize( scaleSizeU * scaleSizeV );
    patches[i].getDepth( 1 ).resize( scaleSizeU * scaleSizeV );
    for ( size_t v = 0; v < scaleSizeV; v++ ) {
      for ( size_t u = 0; u < scaleSizeU; u++ ) {
        size_t p = v * scaleSizeU + u;
        if ( depth[0][p] == infiniteDepth ) {
          patches[i].getDepth( 0 )[p] = infiniteDepth;
          patches[i].getDepth( 1 )[p] = infiniteDepth;
        } else {
          size_t u0 = u / patches[i].getOccupancyResolution();
          size_t v0 = v / patches[i].getOccupancyResolution();
          size_t p0 = v0 * patches[i].getSizeU0() + u0;
          assert( u0 >= 0 && u0 < patches[i].getSizeU0() );
          assert( v0 >= 0 && v0 < patches[i].getSizeV0() );

          patches[i].getOccupancy()[p0] = true;
          patches[i].getDepth( 0 )[p]   = depth[0][p];
          patches[i].getDepth( 1 )[p]   = depth[1][p];
        }
      }
    }
  }  // i<3

  return true;
}

bool PCCEncoder::generateGeometryVideo( const PCCPointSet3&                source,
                                        PCCFrameContext&                   frame,
                                        const PCCPatchSegmenter3Parameters segmenterParams,
                                        PCCVideoGeometry&                  videoGeometry,
                                        PCCFrameContext&                   prevFrame,
                                        size_t                             frameIndex,
                                        float&                             distanceSrcRec ) {
  if ( !source.getPointCount() ) { return false; }

  if ( segmenterParams.additionalProjectionPlaneMode_ != 5 ) {
    auto& patches = frame.getPatches();
    patches.reserve( 256 );
    PCCPatchSegmenter3 segmenter;
    segmenter.setNbThread( params_.nbThread_ );
    segmenter.compute( source, frame.getIndex(), segmenterParams, patches, frame.getSrcPointCloudByPatch(),
                       distanceSrcRec );
  } else if ( segmenterParams.additionalProjectionPlaneMode_ == 5 ) {
    SegmentationPartiallyAddtinalProjectionPlane( source, frame, segmenterParams, videoGeometry, prevFrame, frameIndex,
                                                  distanceSrcRec );
  }

  if ( params_.levelOfDetailX_ > 1 || params_.levelOfDetailY_ > 1 ) { generateScaledGeometry( source, frame ); }

  if ( params_.occupancyMapRefinement_ ) { refineOccupancyMap( frame ); }
  if ( frame.getRawPatchEnabledFlag() ) {
    generateMissedPointsPatch( source, frame,
                               segmenterParams.useEnhancedDeltaDepthCode_ );  // useEnhancedDeltaDepthCode for EDD code
    for ( int i = 0; i < frame.getNumberOfMissedPointsPatches(); i++ ) {
      if ( params_.mortonOrderSortMissedPoints_ ) {
        sortMissedPointsPatchMorton( frame, i );
      } else {
        sortMissedPointsPatch( frame, i );
      }
    }
  }

  if ( params_.enhancedDeltaDepthCode_ ) { generateEomPatch( source, frame ); }

  if ( params_.packingStrategy_ == 0 ) {
    if ( ( frameIndex == 0 ) || ( !params_.constrainedPack_ ) ) {
      pack( frame, params_.safeGuardDistance_, params_.enablePointCloudPartitioning_ );
    } else {
      spatialConsistencyPack( frame, prevFrame, params_.safeGuardDistance_, params_.enablePointCloudPartitioning_ );
    }
  } else {
    if ( params_.packingStrategy_ == 1 ) {
      if ( ( frameIndex == 0 ) || ( !params_.constrainedPack_ ) ) {
        packFlexible( frame, params_.safeGuardDistance_, params_.enablePointCloudPartitioning_ );
      } else {
        if ( params_.globalPatchAllocation_ == 2 )
          findMatchesForGlobalTetrisPacking(
              frame, prevFrame );  // this could also be a different prevFrame, it depends on the prediction structure
        else
          spatialConsistencyPackFlexible( frame, prevFrame, params_.safeGuardDistance_,
                                          params_.enablePointCloudPartitioning_ );
      }
    } else {
      if ( params_.packingStrategy_ == 2 ) {
        if ( ( frameIndex == 0 ) || ( !params_.constrainedPack_ ) ) {
          packTetris( frame, params_.safeGuardDistance_ );
        } else {
          if ( params_.globalPatchAllocation_ == 2 )
            findMatchesForGlobalTetrisPacking(
                frame, prevFrame );  // this could also be a different prevFrame, it depends on the prediction structure
          else
            spatialConsistencyPackTetris( frame, prevFrame, params_.safeGuardDistance_ );
        }
      }
    }
  }
  return true;
}

void PCCEncoder::geometryGroupDilation( PCCContext& context ) {
  auto& videoGeometry     = context.getVideoGeometry();
  auto& videoGeometryD1   = context.getVideoGeometryD1();
  auto  videoOccupancyMap = context.getVideoOccupancyMap();
  auto& frames            = context.getFrames();
  for ( size_t f = 0; f < frames.size(); ++f ) {
    auto& frame        = frames[f];
    auto& width        = frame.getWidth();
    auto& height       = frame.getHeight();
    auto& occupancyMap = videoOccupancyMap.getFrame( f );
    auto& frame1       = params_.multipleStreams_ ? videoGeometry.getFrame( f ) : videoGeometry.getFrame( 2 * f );
    auto& frame2       = params_.multipleStreams_ ? videoGeometryD1.getFrame( f ) : videoGeometry.getFrame( 2 * f + 1 );
    for ( size_t y = 0; y < height; y++ ) {
      for ( size_t x = 0; x < width; x++ ) {
        // const size_t pos = y * width + x;
        if ( occupancyMap.getValue( 0, x / params_.occupancyPrecision_, y / params_.occupancyPrecision_ ) == 0 ) {
          uint32_t avg =
              ( ( (uint32_t)frame1.getValue( 0, x, y ) ) + ( (uint32_t)frame2.getValue( 0, x, y ) ) + 1 ) >> 1;
          frame1.setValue( 0, x, y, (uint16_t)avg );
          frame2.setValue( 0, x, y, (uint16_t)avg );
        }
      }
    }
  }
}

bool PCCEncoder::generateOccupancyMap( PCCContext& context ) {
  for ( auto& frame : context.getFrames() ) {
    generateOccupancyMap( frame );
    if ( params_.enhancedDeltaDepthCode_ ) { modifyOccupancyMapEDD( frame ); }
  }
  return true;
}

void PCCEncoder::generateOccupancyMap( PCCFrameContext& frame ) {
  auto& occupancyMap     = frame.getOccupancyMap();
  auto& fullOccupancyMap = frame.getFullOccupancyMap();
  auto& width            = frame.getWidth();
  auto& height           = frame.getHeight();
  occupancyMap.resize( width * height, 0 );
  if ( !params_.absoluteD1_ || !params_.absoluteT1_ ) { fullOccupancyMap.resize( width * height, 0 ); }
  //  const int16_t infiniteDepth = ( std::numeric_limits<int16_t>::max )();
  for ( auto& patch : frame.getPatches() ) {
    for ( size_t v = 0; v < patch.getSizeV(); ++v ) {
      for ( size_t u = 0; u < patch.getSizeU(); ++u ) {
        const size_t  p = v * patch.getSizeU() + u;
        const int16_t d = patch.getDepth( 0 )[p];
        if ( d < infiniteDepth ) {
          size_t x;
          size_t y;
          occupancyMap[patch.patch2Canvas( u, v, width, height, x, y )] = 1;
        }
      }
    }
  }

  if ( !frame.getUseMissedPointsSeparateVideo() ) {
    size_t numberOfMpsPatches = frame.getNumberOfMissedPointsPatches();
    for ( int i = 0; i < numberOfMpsPatches; i++ ) {
      auto&        missedPointsPatch = frame.getMissedPointsPatch( i );
      const size_t v0                = missedPointsPatch.v0_ * missedPointsPatch.occupancyResolution_;
      const size_t u0                = missedPointsPatch.u0_ * missedPointsPatch.occupancyResolution_;
      if ( missedPointsPatch.size() ) {
        for ( size_t v = 0; v < missedPointsPatch.sizeV_; ++v ) {
          for ( size_t u = 0; u < missedPointsPatch.sizeU_; ++u ) {
            const size_t p = v * missedPointsPatch.sizeU_ + u;
            if ( missedPointsPatch.x_[p] < infiniteDepth ) {
              // if (p < missedPointsPatch.getNumberOfMps()) {
              const size_t x = ( u0 + u );
              const size_t y = ( v0 + v );
              if ( x >= width || y >= height ) {
                std::cout << "\t\tout of image :" << x << "," << y << "(" << x + y * width << ") vs occupancyMap size"
                          << occupancyMap.size() << std::endl;
                exit( 0 );
              }
              assert( x < width && y < height );
              occupancyMap[x + y * width] = 1;
            }
          }
        }
      }
    }
  }
  if ( !params_.absoluteD1_ || !params_.absoluteT1_ ) { fullOccupancyMap = occupancyMap; }
}

void PCCEncoder::refineOccupancyMap( PCCFrameContext& frame ) {
  auto&        patches        = frame.getPatches();
  const size_t patchCount     = patches.size();
  size_t       countRemove4x4 = 0, countRemove16x16 = 0;

  for ( size_t patchIndex = 0; patchIndex < patchCount; ++patchIndex ) {
    auto& patch = patches[patchIndex];
    // Count number of points in each block 4x4
    if ( params_.occupancyPrecision_ > 1 ) {
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); v0++ ) {
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); u0++ ) {
          for ( size_t v1 = 0; v1 < params_.occupancyResolution_; v1 += params_.occupancyPrecision_ ) {
            for ( size_t u1 = 0; u1 < params_.occupancyResolution_; u1 += params_.occupancyPrecision_ ) {
              size_t countOccupancyMapBlock4x4 = 0;
              for ( size_t v2 = 0; v2 < params_.occupancyPrecision_; v2++ ) {
                const size_t v = v0 * params_.occupancyResolution_ + v1 + v2;
                if ( v < patch.getSizeV() ) {
                  for ( size_t u2 = 0; u2 < params_.occupancyPrecision_; u2++ ) {
                    const size_t u = u0 * params_.occupancyResolution_ + u1 + u2;
                    if ( u < patch.getSizeU() ) {
                      const size_t p = v * patch.getSizeU() + u;
                      if ( patch.getDepth( 0 )[p] < infiniteDepth ) { countOccupancyMapBlock4x4++; }
                    }
                  }
                }
              }
              if ( countOccupancyMapBlock4x4 > 0 ) {
                if ( countOccupancyMapBlock4x4 == 1 ) {
                  countRemove4x4++;
                  for ( size_t v2 = 0; v2 < params_.occupancyPrecision_; v2++ ) {
                    const size_t v = v0 * params_.occupancyResolution_ + v1 + v2;
                    if ( v < patch.getSizeV() ) {
                      for ( size_t u2 = 0; u2 < params_.occupancyPrecision_; u2++ ) {
                        const size_t u = u0 * params_.occupancyResolution_ + u1 + u2;
                        if ( u < patch.getSizeU() ) {
                          const size_t p         = v * patch.getSizeU() + u;
                          patch.getDepth( 0 )[p] = infiniteDepth;
                          patch.getDepth( 1 )[p] = infiniteDepth;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    // Count number of points in each block 16x16
    for ( size_t v0 = 0; v0 < patch.getSizeV0(); v0++ ) {
      for ( size_t u0 = 0; u0 < patch.getSizeU0(); u0++ ) {
        size_t countOccupancyMapBlock16x16 = 0;
        for ( size_t v1 = 0; v1 < params_.occupancyResolution_; ++v1 ) {
          const size_t v = v0 * params_.occupancyResolution_ + v1;
          if ( v < patch.getSizeV() ) {
            for ( size_t u1 = 0; u1 < params_.occupancyResolution_; ++u1 ) {
              const size_t u = u0 * params_.occupancyResolution_ + u1;
              if ( u < patch.getSizeU() ) {
                const size_t p      = v * patch.getSizeU() + u;
                int16_t      depth0 = patch.getDepth( 0 )[p];
                if ( depth0 < infiniteDepth ) { countOccupancyMapBlock16x16++; }
              }
            }
          }
        }
        if ( countOccupancyMapBlock16x16 == 0 ) {
          patch.getOccupancy()[v0 * patch.getSizeU0() + u0] = false;
        } else {
          if ( countOccupancyMapBlock16x16 < 4 && countOccupancyMapBlock16x16 != 0 ) {
            countRemove16x16++;
            patch.getOccupancy()[v0 * patch.getSizeU0() + u0] = false;
            // remove block 16x16
            for ( size_t v1 = 0; v1 < params_.occupancyResolution_; ++v1 ) {
              const size_t v = v0 * params_.occupancyResolution_ + v1;
              if ( v < patch.getSizeV() ) {
                for ( size_t u1 = 0; u1 < params_.occupancyResolution_; ++u1 ) {
                  const size_t u = u0 * params_.occupancyResolution_ + u1;
                  if ( u < patch.getSizeU() ) {
                    const size_t p         = v * patch.getSizeU() + u;
                    patch.getDepth( 0 )[p] = infiniteDepth;
                    patch.getDepth( 1 )[p] = infiniteDepth;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

void PCCEncoder::remove3DMotionEstimationFiles( std::string path ) {
  removeFile( path + "occupancy.txt" );
  removeFile( path + "patchInfo.txt" );
  removeFile( path + "blockToPatch.txt" );
}

void PCCEncoder::create3DMotionEstimationFiles( const PCCGroupOfFrames& sources,
                                                PCCContext&             context,
                                                std::string             path ) {
  FILE* occupancyFile    = fopen( ( path + "occupancy.txt" ).c_str(), "wb" );
  FILE* patchInfoFile    = fopen( ( path + "patchInfo.txt" ).c_str(), "wb" );
  FILE* blockToPatchFile = fopen( ( path + "blockToPatch.txt" ).c_str(), "wb" );

  for ( size_t frIdx = 0; frIdx < sources.size(); ++frIdx ) {
    auto&        frame              = context.getFrame( frIdx );
    auto&        occupancyMapImage  = context.getVideoOccupancyMap().getFrame( frIdx );
    auto&        patches            = frame.getPatches();
    auto&        blockToPatch       = frame.getBlockToPatch();
    const size_t blockToPatchWidth  = frame.getWidth() / params_.occupancyResolution_;
    const size_t blockToPatchHeight = frame.getHeight() / params_.occupancyResolution_;
    fwrite( &blockToPatch[0], sizeof( size_t ), blockToPatchHeight * blockToPatchWidth, blockToPatchFile );
    // fwrite( &occupancyMap[0], sizeof( uint32_t ), frame.getHeight() * frame.getWidth(), occupancyFile );
    uint32_t zeroVal = 0;
    uint32_t oneVal  = 1;
    for ( int y = 0; y < frame.getHeight(); y++ ) {
      for ( int x = 0; x < frame.getWidth(); x++ ) {
        if ( occupancyMapImage.getValue( 0, x / params_.occupancyPrecision_, y / params_.occupancyPrecision_ ) > 0 )
          fwrite( &oneVal, sizeof( uint32_t ), 1, occupancyFile );
        else
          fwrite( &zeroVal, sizeof( uint32_t ), 1, occupancyFile );
      }
    }
    const size_t numPatches = patches.size();
    fwrite( &numPatches, sizeof( size_t ), 1, patchInfoFile );
    for ( const auto& patch : patches ) {
      size_t projectionIndex = patch.getNormalAxis();
      size_t u0              = patch.getU0();
      size_t v0              = patch.getV0();
      size_t sizeU0          = patch.getSizeU0();
      size_t sizeV0          = patch.getSizeV0();
      size_t d1              = patch.getD1();
      size_t u1              = patch.getU1();
      size_t v1              = patch.getV1();
      fwrite( &projectionIndex, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &u0, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &v0, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &sizeU0, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &sizeV0, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &d1, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &u1, sizeof( size_t ), 1, patchInfoFile );
      fwrite( &v1, sizeof( size_t ), 1, patchInfoFile );
    }
  }
  fclose( blockToPatchFile );
  fclose( occupancyFile );
  fclose( patchInfoFile );
}

void PCCEncoder::generateIntraImage( PCCFrameContext& frame, const size_t mapIndex, PCCImageGeometry& image ) {
  auto& width  = frame.getWidth();
  auto& height = frame.getHeight();
  image.resize( width, height );
  image.set( 0 );
  //  const int16_t infiniteDepth = ( std::numeric_limits<int16_t>::max )();
  size_t maxDepth = 0;
  for ( auto& patch : frame.getPatches() ) {
    for ( size_t v = 0; v < patch.getSizeV(); ++v ) {
      for ( size_t u = 0; u < patch.getSizeU(); ++u ) {
        const size_t  p = v * patch.getSizeU() + u;
        const int16_t d = patch.getDepth( mapIndex )[p];
        if ( d < infiniteDepth ) {
          size_t x, y;
          patch.patch2Canvas( u, v, width, height, x, y );
          // FIX1
          //          if ( msb_align_flag )
          //            // image.setValue(0, x, y, uint16_t(d));
          image.setValue( 0, x, y, uint16_t( d ) );
          maxDepth = ( std::max )( maxDepth, patch.getSizeD() );
        }
      }
    }
  }

  if ( maxDepth >= ( 1 << frame.getGeometry2dNorminalBitdepth() ) ) {
    std::cout << "Error: maxDepth(" << maxDepth << ") >=" << ( 1 << frame.getGeometry2dNorminalBitdepth() )
              << std::endl;
    exit( -1 );
  }

  if ( !frame.getUseMissedPointsSeparateVideo() ) {
    size_t numberOfMpsPatches = frame.getNumberOfMissedPointsPatches();
    for ( int i = 0; i < numberOfMpsPatches; i++ ) {
      auto&        missedPointsPatch = frame.getMissedPointsPatch( i );
      const size_t v0                = missedPointsPatch.v0_ * missedPointsPatch.occupancyResolution_;
      const size_t u0                = missedPointsPatch.u0_ * missedPointsPatch.occupancyResolution_;
      if ( missedPointsPatch.size() ) {
        for ( size_t v = 0; v < missedPointsPatch.sizeV_; ++v ) {
          for ( size_t u = 0; u < missedPointsPatch.sizeU_; ++u ) {
            const size_t p = v * missedPointsPatch.sizeU_ + u;
            if ( missedPointsPatch.x_[p] < infiniteDepth ) {
              const size_t x = ( u0 + u );
              const size_t y = ( v0 + v );
              assert( x < width && y < height );
              image.setValue( 0, x, y, uint16_t( missedPointsPatch.x_[p] ) );
              if ( params_.losslessGeo444_ ) {
                image.setValue( 1, x, y, uint16_t( missedPointsPatch.y_[p] ) );
                image.setValue( 2, x, y, uint16_t( missedPointsPatch.z_[p] ) );
              }
            }
          }
        }
      }
    }
  }
}
bool PCCEncoder::predictTextureFrame( PCCFrameContext&       frame,
                                      const PCCImageTexture& reference,
                                      PCCImageTexture&       image ) {
  assert( reference.getWidth() == image.getWidth() );
  assert( reference.getHeight() == image.getHeight() );
  const size_t refWidth     = reference.getWidth();
  const size_t refHeight    = reference.getHeight();
  auto&        occupancyMap = frame.getOccupancyMap();
  for ( size_t y = 0; y < refHeight; ++y ) {
    for ( size_t x = 0; x < refWidth; ++x ) {
      const size_t pos1 = y * refWidth + x;
      if ( occupancyMap[pos1] != 0 ) {
        for ( size_t c = 0; c < 3; ++c ) {
          const int16_t value1 = static_cast<int16_t>( image.getValue( c, x, y ) );
          const int16_t value0 = static_cast<int16_t>( reference.getValue( c, x, y ) );
          int16_t       delta  = 0;
          delta                = value1 - value0;
          if ( delta < -128 ) {
            delta = -128;
          } else if ( delta > 127 ) {
            delta = 127;
          }
          delta += 128;
          delta = delta < 0 ? 0 : ( delta > 255 ? 255 : delta );
          image.setValue( c, x, y, ( uint8_t )( delta ) );
        }  // c
      } else {
        image.setValue( 0, x, y, ( uint8_t )( 128 ) );
        image.setValue( 1, x, y, ( uint8_t )( 128 ) );
        image.setValue( 2, x, y, ( uint8_t )( 128 ) );
      }
    }
  }
  return true;
}

bool PCCEncoder::predictGeometryFrame( PCCFrameContext&        frame,
                                       const PCCImageGeometry& reference,
                                       PCCImageGeometry&       image ) {
  assert( reference.getWidth() == image.getWidth() );
  assert( reference.getHeight() == image.getHeight() );

  auto& patches      = frame.getPatches();
  auto& blockToPatch = frame.getBlockToPatch();
  auto& occupancyMap = frame.getFullOccupancyMap();

  const size_t imageWidth  = reference.getWidth();
  const size_t imageHeight = reference.getHeight();

  const size_t blockToPatchWidth  = frame.getWidth() / params_.occupancyResolution_;
  const size_t blockToPatchHeight = frame.getHeight() / params_.occupancyResolution_;

  const size_t patchCount = patches.size();
  size_t       patchIndex{0};

  for ( patchIndex = 0; patchIndex < patchCount; ++patchIndex ) {
    const size_t patchIndexPlusOne = patchIndex + 1;
    auto&        patch             = patches[patchIndex];

    for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
      for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
        const size_t blockIndex = patch.patchBlock2CanvasBlock( u0, v0, blockToPatchWidth, blockToPatchHeight );
        if ( blockToPatch[blockIndex] == patchIndexPlusOne ) {
          for ( size_t v1 = 0; v1 < patch.getOccupancyResolution(); ++v1 ) {
            const size_t v = v0 * patch.getOccupancyResolution() + v1;
            for ( size_t u1 = 0; u1 < patch.getOccupancyResolution(); ++u1 ) {
              const size_t u = u0 * patch.getOccupancyResolution() + u1;
              size_t       x, y;
              patch.patch2Canvas( u, v, imageWidth, imageHeight, x, y );
              const bool occupancy = occupancyMap[y * imageWidth + x] != 0;
              if ( !occupancy ) { continue; }
              const uint16_t value1 = static_cast<uint16_t>( image.getValue( 0, x, y ) );
              const uint16_t value0 = static_cast<uint16_t>( reference.getValue( 0, x, y ) );
              int_least32_t  delta  = 0;
              delta                 = std::abs( (int_least32_t)value1 - (int_least32_t)value0 );
              if ( delta < 0 ) { delta = 0; }
              if ( !params_.losslessGeo_ && delta > 9 ) { delta = 9; }
              image.setValue( 0, x, y, (uint8_t)delta );
            }
          }
        }
      }
    }
  }

  return true;
}

void PCCEncoder::generateEomPatch( const PCCPointSet3& source, PCCFrameContext& frame ) {
  auto& eomPatches = frame.getEomPatches();
  eomPatches.resize( 1 );
  size_t patchCount    = frame.getPatches().size();
  size_t totalEddCount = 0;
  for ( size_t patchIdx = 0; patchIdx < patchCount; patchIdx++ ) {
    auto&  patch            = frame.getPatches()[patchIdx];
    size_t eddCountPerPatch = 0;
    totalEddCount += patch.getEddCount();
    for ( size_t v = 0; v < patch.getSizeV(); ++v ) {
      for ( size_t u = 0; u < patch.getSizeU(); ++u ) {
        const size_t p       = v * patch.getSizeU() + u;
        int16_t      eddCode = patch.getDepthEnhancedDeltaD()[p];
        if ( eddCode ) {
          uint16_t nbBits = 0;
          for ( uint16_t i = 0; i < 10; i++ ) {
            if ( eddCode & ( 1 << i ) ) nbBits++;
          }
          if ( params_.mapCountMinus1_ > 0 ) nbBits--;  // don't count d1
          eddCountPerPatch += nbBits;
        }
      }
    }
    eomPatches[0].memberPatches.push_back( patchIdx );
    eomPatches[0].eddCountPerPatch.push_back( patch.getEddCount() );
    assert( patch.getEddCount() == eddCountPerPatch );
    patch.setEddCount( eddCountPerPatch );
  }
  eomPatches[0].eddCount_ = totalEddCount;
}

void PCCEncoder::generateMissedPointsPatch( const PCCPointSet3& source,
                                            PCCFrameContext&    frame,
                                            bool                useEnhancedDeltaDepthCode ) {
  //  const int16_t infiniteDepth    = ( std::numeric_limits<int16_t>::max )();
  auto& patches = frame.getPatches();

  const size_t geometry3dCoordinatesBitdepth = params_.geometry3dCoordinatesBitdepth_;

  PCCPointSet3 pointsToBeProjected;
  for ( const auto& patch : patches ) {
    for ( size_t v = 0; v < patch.getSizeV(); ++v ) {
      for ( size_t u = 0; u < patch.getSizeU(); ++u ) {
        const size_t p      = v * patch.getSizeU() + u;
        const size_t depth0 = patch.getDepth( 0 )[p];
        if ( depth0 < infiniteDepth ) {
          PCCPoint3D point0;

          if ( patch.getProjectionMode() == 0 ) {
            point0[patch.getNormalAxis()] = double( depth0 + patch.getD1() );
          } else {
            point0[patch.getNormalAxis()] = double( patch.getD1() - depth0 );
          }

          point0[patch.getTangentAxis()]   = double( u ) + patch.getU1();
          point0[patch.getBitangentAxis()] = double( v ) + patch.getV1();
          if ( patch.getAxisOfAdditionalPlane() != 0 ) {
            PCCPoint3D  input = point0;
            PCCVector3D tmp1;
            PCCPatch::InverseRotatePosition45DegreeOnAxis( patch.getAxisOfAdditionalPlane(),
                                                           geometry3dCoordinatesBitdepth, input, tmp1 );
            point0.x() = tmp1.x();
            point0.y() = tmp1.y();
            point0.z() = tmp1.z();
          }
          pointsToBeProjected.addPoint( point0 );
          if ( useEnhancedDeltaDepthCode ) {
            if ( patch.getDepthEnhancedDeltaD()[p] != 0 ) {
              PCCPoint3D point1;
              point1[patch.getTangentAxis()]   = double( u ) + patch.getU1();
              point1[patch.getBitangentAxis()] = double( v ) + patch.getV1();
              for ( uint16_t i = 0; i < 16; i++ ) {  // surfaceThickness is not necessary here?
                if ( patch.getDepthEnhancedDeltaD()[p] & ( 1 << i ) ) {
                  uint16_t nDeltaDCur = ( i + 1 );
                  size_t   depth1     = 0;
                  if ( params_.mapCountMinus1_ == 0 ) {
                    depth1 = depth0;
                    if ( params_.mapCountMinus1_ > 0 ) { depth1 = patch.getDepth( 1 )[p]; }

                    if ( params_.mapCountMinus1_ > 0 && depth0 + nDeltaDCur >= depth1 ) { nDeltaDCur++; }

                  } else {
                    depth1 = patch.getDepth( 1 )[p];
                  }

                  if ( patch.getProjectionMode() == 0 ) {
                    point1[patch.getNormalAxis()] = double( depth0 + patch.getD1() + nDeltaDCur );
                  } else {
                    point1[patch.getNormalAxis()] = double( patch.getD1() - depth0 - nDeltaDCur );
                  }
                  pointsToBeProjected.addPoint( point1 );
                }
              }  // for each i
            }    // if( patch.getDepthEnhancedDeltaD()[p] != 0) )
          } else {
            const size_t depth1 = patch.getDepth( 1 )[p];
            PCCPoint3D   point1;
            point1[patch.getTangentAxis()]   = double( u ) + patch.getU1();
            point1[patch.getBitangentAxis()] = double( v ) + patch.getV1();
            if ( patch.getProjectionMode() == 0 ) {
              point1[patch.getNormalAxis()] = double( depth1 ) + patch.getD1();
            } else {
              point1[patch.getNormalAxis()] = double( patch.getD1() ) - double( depth1 );
            }
            if ( patch.getAxisOfAdditionalPlane() != 0 ) {
              PCCPoint3D  input = point1;
              PCCVector3D tmp3;
              PCCPatch::InverseRotatePosition45DegreeOnAxis( patch.getAxisOfAdditionalPlane(),
                                                             geometry3dCoordinatesBitdepth, input, tmp3 );
              point1.x() = tmp3.x();
              point1.y() = tmp3.y();
              point1.z() = tmp3.z();
            }
            pointsToBeProjected.addPoint( point1 );
          }
        }
      }
    }
  }
  PCCKdTree           kdtreeMissedPoints( pointsToBeProjected );
  PCCNNResult         result;
  std::vector<size_t> missedPoints;
  missedPoints.resize( 0 );
  for ( size_t i = 0; i < source.getPointCount(); ++i ) {
    kdtreeMissedPoints.search( source[i], 1, result );
    const double dist2 = result.dist( 0 );
    if ( dist2 > 0.0 ) { missedPoints.push_back( i ); }
  }
  size_t numMissedPts = missedPoints.size();

  if ( params_.lossyMissedPointsPatch_ ) {
    // Settings for selecting/pruning points.
    const size_t maxNeighborCount                      = 16;
    const size_t maxDist                               = 10;  // lower the value of maxDist, fewer points will selected
    const double minSumOfInvDist4MissedPointsSelection = params_.minNormSumOfInvDist4MPSelection_ * maxNeighborCount;
    std::vector<size_t> tmpMissedPoints;
    tmpMissedPoints.resize( 0 );
    PCCPointSet3 missedPointsSet;
    missedPointsSet.resize( numMissedPts );
    // create missed points cloud
    for ( size_t i = 0; i < numMissedPts; ++i ) { missedPointsSet[i] = source[missedPoints[i]]; }
    PCCKdTree kdtreeMissedPointsSet( missedPointsSet );
    double    sumOfInverseDist = 0.0;
    for ( size_t i = 0; i < numMissedPts; ++i ) {
      PCCNNResult result;
      kdtreeMissedPointsSet.searchRadius( missedPointsSet[i], maxNeighborCount, maxDist, result );
      sumOfInverseDist = 0.0;
      for ( size_t j = 1; j < result.count(); ++j ) { sumOfInverseDist += 1 / result.dist( j ); }
      if ( sumOfInverseDist >= minSumOfInvDist4MissedPointsSelection ) { tmpMissedPoints.push_back( missedPoints[i] ); }
    }
    numMissedPts = tmpMissedPoints.size();
    missedPoints.resize( numMissedPts );
    missedPoints = tmpMissedPoints;
  }

  frame.setTotalNumberOfMissedPoints( numMissedPts );
  const int16_t infiniteValue = ( std::numeric_limits<int16_t>::max )();
  frame.setTotalNumberOfMissedPoints( missedPoints.size() );
  std::cout << "missedPoints.size() = " << missedPoints.size() << std::endl;
  PCCBox3D inputBbox = source.computeBoundingBox();
  inputBbox.min_.x() = 0;
  inputBbox.min_.y() = 0;
  inputBbox.min_.z() = 0;
  std::cout << "input boundinBox::(min_x, min_y, min_z) = (" << inputBbox.min_.x() << ", " << inputBbox.min_.y() << ", "
            << inputBbox.min_.z() << ");" << std::endl;
  std::cout << "missedPoints:==============================" << std::endl;
  std::cout << "input boundinBox::(max_x, max_y, max_z) = (" << inputBbox.max_.x() << ", " << inputBbox.max_.y() << ", "
            << inputBbox.max_.z() << ");" << std::endl;

  PCCBox3D bboxMps;
  double   mpsBoxSize = double( 1 << params_.geometryNominal2dBitdepth_ );

  bboxMps.min_ = inputBbox.min_;
  bboxMps.max_ = inputBbox.min_;
  bboxMps.max_.x() += mpsBoxSize;
  bboxMps.max_.y() += mpsBoxSize;
  bboxMps.max_.z() += mpsBoxSize;
  bool   isEmptyBox            = true;
  size_t numberOfMissedPatches = 0;

  for ( bboxMps.min_.x() = inputBbox.min_.x(); bboxMps.min_.x() <= inputBbox.max_.x();
        bboxMps.min_.x() += mpsBoxSize ) {
    bboxMps.max_.x() = bboxMps.min_.x() + ( mpsBoxSize - 1 );
    for ( bboxMps.min_.y() = inputBbox.min_.y(); bboxMps.min_.y() <= inputBbox.max_.y();
          bboxMps.min_.y() += mpsBoxSize ) {
      bboxMps.max_.y() = bboxMps.min_.y() + ( mpsBoxSize - 1 );
      for ( bboxMps.min_.z() = inputBbox.min_.z(); bboxMps.min_.z() <= inputBbox.max_.z();
            bboxMps.min_.z() += mpsBoxSize ) {
        bboxMps.max_.z() = bboxMps.min_.z() + ( mpsBoxSize - 1 );
        isEmptyBox       = source.isMissedBboxEmpty( missedPoints, bboxMps );
        if ( !isEmptyBox ) {
          std::cout << "box(Xmin,Ymin,Zmin,Xmax,Ymax,Zmax) = ( " << bboxMps.min_.x() << ", " << bboxMps.min_.y() << ", "
                    << bboxMps.min_.z() << ", " << bboxMps.max_.x() << ", " << bboxMps.max_.y() << ", "
                    << bboxMps.max_.z() << ") " << std::endl;
          numberOfMissedPatches++;
          std::cout << "bboxMps::numberOfMissedPatches = " << frame.getNumberOfMissedPointsPatches() << std::endl;
          auto&                mpsPatches = frame.getMissedPointsPatches();
          PCCMissedPointsPatch missedPointsPatch;
          std::vector<size_t>  bboxMissedPoints;
          source.fillMissedPointsBbox( missedPoints, bboxMps, bboxMissedPoints );
          const size_t mps = bboxMissedPoints.size();
          frame.getNumberOfMissedPoints().resize( numberOfMissedPatches );
          frame.setNumberOfMissedPoints( numberOfMissedPatches - 1, mps );

          missedPointsPatch.occupancyResolution_ = params_.occupancyResolution_;
          missedPointsPatch.sizeU_               = 0;
          missedPointsPatch.sizeV_               = 0;
          missedPointsPatch.u0_                  = 0;
          missedPointsPatch.v0_                  = 0;
          missedPointsPatch.sizeV0_              = 0;
          missedPointsPatch.sizeU0_              = 0;
          missedPointsPatch.u1_                  = size_t( bboxMps.min_.x() );
          missedPointsPatch.v1_                  = size_t( bboxMps.min_.y() );
          missedPointsPatch.d1_                  = size_t( bboxMps.min_.z() );
          missedPointsPatch.occupancy_.resize( 0 );
          missedPointsPatch.setNumberOfMps( mps );
          if ( params_.losslessGeo444_ ) {
            missedPointsPatch.resize( 3 * mps );
            for ( auto i = 0; i < mps; ++i ) {
              const PCCPoint3D missedPoint = source[bboxMissedPoints[i]];
              missedPointsPatch.x_[i]      = static_cast<uint16_t>( missedPoint.x() - missedPointsPatch.u1_ );
              missedPointsPatch.y_[i]      = static_cast<uint16_t>( missedPoint.y() - missedPointsPatch.v1_ );
              missedPointsPatch.z_[i]      = static_cast<uint16_t>( missedPoint.z() - missedPointsPatch.d1_ );
            }
          } else {
            missedPointsPatch.resize( 3 * mps );
            for ( auto i = 0; i < mps; ++i ) {
              const PCCPoint3D missedPoint      = source[bboxMissedPoints[i]];
              missedPointsPatch.x_[i]           = static_cast<uint16_t>( missedPoint.x() - missedPointsPatch.u1_ );
              missedPointsPatch.x_[mps + i]     = static_cast<uint16_t>( missedPoint.y() - missedPointsPatch.v1_ );
              missedPointsPatch.x_[2 * mps + i] = static_cast<uint16_t>( missedPoint.z() - missedPointsPatch.d1_ );
              missedPointsPatch.y_[i]           = infiniteValue;
              missedPointsPatch.y_[mps + i]     = infiniteValue;
              missedPointsPatch.y_[2 * mps + i] = infiniteValue;
              missedPointsPatch.z_[i]           = infiniteValue;
              missedPointsPatch.z_[mps + i]     = infiniteValue;
              missedPointsPatch.z_[2 * mps + i] = infiniteValue;
            }
          }
          mpsPatches.push_back( missedPointsPatch );
        }
      }
    }
  }
}

void PCCEncoder::sortMissedPointsPatchMorton( PCCFrameContext& frame, size_t index ) {
  auto&  missedPointsPatch = frame.getMissedPointsPatch( index );
  size_t numMissedPts      = missedPointsPatch.getNumberOfMps();
  if ( numMissedPts ) {
    PCCPointSet3 missedPointSet;
    missedPointSet.resize( numMissedPts );
    for ( size_t i = 0; i < numMissedPts; i++ ) {
      missedPointSet[i] = params_.losslessGeo444_
                              ? PCCPoint3D( missedPointsPatch.x_[i], missedPointsPatch.y_[i], missedPointsPatch.z_[i] )
                              : PCCPoint3D( missedPointsPatch.x_[i], missedPointsPatch.x_[i + numMissedPts],
                                            missedPointsPatch.x_[i + numMissedPts * 2] );
    }
    // calc Morton code of missedPointSet
    std::vector<std::pair<uint64_t, PCCPoint3D>> mortonPoint;
    mortonPoint.resize( numMissedPts );
    for ( size_t i = 0; i < numMissedPts; ++i ) {
      mortonPoint[i].first  = mortonAddr( missedPointSet[i], 0 );
      mortonPoint[i].second = missedPointSet[i];
    }
    // sort points according to their Morton codes
    std::sort( mortonPoint.begin(), mortonPoint.end() );
    for ( size_t i = 0; i < numMissedPts; ++i ) {
      const PCCPoint3D missedPoint = mortonPoint[i].second;
      if ( params_.losslessGeo444_ ) {
        missedPointsPatch.x_[i] = static_cast<uint16_t>( missedPoint.x() );
        missedPointsPatch.y_[i] = static_cast<uint16_t>( missedPoint.y() );
        missedPointsPatch.z_[i] = static_cast<uint16_t>( missedPoint.z() );
      } else {
        missedPointsPatch.x_[i]                    = static_cast<uint16_t>( missedPoint.x() );
        missedPointsPatch.x_[i + numMissedPts]     = static_cast<uint16_t>( missedPoint.y() );
        missedPointsPatch.x_[i + numMissedPts * 2] = static_cast<uint16_t>( missedPoint.z() );
      }
    }
  }
}

void PCCEncoder::sortMissedPointsPatch( PCCFrameContext& frame, size_t index ) {
  auto&        missedPointsPatch    = frame.getMissedPointsPatch( index );
  const size_t maxNeighborCount     = 5;
  const size_t neighborSearchRadius = 5 * 5;
  size_t       numMissedPts         = missedPointsPatch.getNumberOfMps();
  if ( numMissedPts ) {
    vector<size_t> sortIdx;
    sortIdx.reserve( numMissedPts );
    PCCPointSet3 missedPointSet;
    missedPointSet.resize( numMissedPts );
    for ( size_t i = 0; i < numMissedPts; i++ ) {
      missedPointSet[i] = params_.losslessGeo444_
                              ? PCCPoint3D( missedPointsPatch.x_[i], missedPointsPatch.y_[i], missedPointsPatch.z_[i] )
                              : PCCPoint3D( missedPointsPatch.x_[i], missedPointsPatch.x_[i + numMissedPts],
                                            missedPointsPatch.x_[i + numMissedPts * 2] );
    }
    PCCKdTree           kdtreeMissedPointSet( missedPointSet );
    PCCNNResult         result;
    std::vector<size_t> fifo;
    fifo.reserve( numMissedPts );
    std::vector<bool> flags( numMissedPts, true );

    for ( size_t i = 0; i < numMissedPts; i++ ) {
      if ( flags[i] ) {
        flags[i] = false;
        sortIdx.push_back( i );
        fifo.push_back( i );
        while ( !fifo.empty() ) {
          const size_t currentIdx = fifo.back();
          fifo.pop_back();
          kdtreeMissedPointSet.searchRadius( missedPointSet[currentIdx], maxNeighborCount, neighborSearchRadius,
                                             result );
          for ( size_t j = 0; j < result.count(); j++ ) {
            size_t n = result.indices( j );
            if ( flags[n] ) {
              flags[n] = false;
              sortIdx.push_back( n );
              fifo.push_back( n );
            }
          }
        }
      }
    }

    for ( size_t i = 0; i < numMissedPts; ++i ) {
      const PCCPoint3D missedPoint = missedPointSet[sortIdx[i]];
      if ( params_.losslessGeo444_ ) {
        missedPointsPatch.x_[i] = static_cast<uint16_t>( missedPoint.x() );
        missedPointsPatch.y_[i] = static_cast<uint16_t>( missedPoint.y() );
        missedPointsPatch.z_[i] = static_cast<uint16_t>( missedPoint.z() );
      } else {
        missedPointsPatch.x_[i]                    = static_cast<uint16_t>( missedPoint.x() );
        missedPointsPatch.x_[i + numMissedPts]     = static_cast<uint16_t>( missedPoint.y() );
        missedPointsPatch.x_[i + numMissedPts * 2] = static_cast<uint16_t>( missedPoint.z() );
      }
    }
  }
}

void PCCEncoder::generateMissedPointsGeometryVideo( PCCContext& context, PCCGroupOfFrames& reconstructs ) {
  auto&  sps              = context.getSps();
  auto&  videoMPsGeometry = context.getVideoMPsGeometry();
  auto   gofSize          = context.size();
  size_t maxWidth         = 0;
  size_t maxHeight        = 0;
  videoMPsGeometry.resize( gofSize );
  for ( auto& frame : context.getFrames() ) {
    const size_t shift = frame.getIndex();
    frame.setLosslessGeo( sps.getLosslessGeo() );
    frame.setLosslessGeo444( sps.getLosslessGeo444() );
    frame.setMPGeoWidth( context.getMPGeoWidth() );
    printf( "generateMissedPointsGeometryVideo::context.getMPGeoWidth() = %zu \n", context.getMPGeoWidth() );
    frame.setMPGeoHeight( 0 );
    // frame.setEnhancedDeltaDepth( sps.getEnhancedOccupancyMapForDepthFlag());
    generateMPsGeometryImage( context, frame, videoMPsGeometry.getFrame( shift ) );

    size_t totalNumRawPoints = 0;
    for ( size_t ii = 0; ii < frame.getNumberOfMissedPointsPatches(); ii++ )
      totalNumRawPoints += frame.getMissedPointsPatch( ii ).size();
    cout << "generate raw Points Video (Geometry) frame " << shift
         << ": # of raw Patches : " << frame.getNumberOfMissedPointsPatches()
         << " total # of raw Geometry : " << totalNumRawPoints << endl;

    // for resizing for mpgeometry
    auto& MPGeoFrame = videoMPsGeometry.getFrame( shift );
    maxWidth         = ( std::max )( maxWidth, MPGeoFrame.getWidth() );
    maxHeight        = ( std::max )( maxHeight, MPGeoFrame.getHeight() );
  }

  // resizing for mpgeometry
  assert( maxWidth == 64 );
  assert( maxHeight % 8 == 0 );
  context.setMPGeoWidth( maxWidth );
  context.setMPGeoHeight( maxHeight );
  for ( auto& frame : context.getFrames() ) {
    const size_t shift      = frame.getIndex();
    auto&        MPGeoFrame = videoMPsGeometry.getFrame( shift );
    MPGeoFrame.resize( maxWidth, maxHeight );
  }
  cout << "generateRawPointsGeometryVideo [done]" << endl;
}

void PCCEncoder::generateMissedPointsTextureVideo( PCCContext& context, PCCGroupOfFrames& reconstructs ) {
  auto& videoMPsTexture = context.getVideoMPsTexture();
  auto  gofSize         = context.size();
  videoMPsTexture.resize( gofSize );
  size_t maxWidth  = 0;
  size_t maxHeight = 0;
  for ( auto& frame : context.getFrames() ) {
    const size_t shift = frame.getIndex();
    frame.setMPAttWidth( context.getMPAttWidth() );
    frame.setMPAttHeight( 0 );
    generateMPsTextureImage( context, frame, videoMPsTexture.getFrame( shift ), shift, reconstructs[shift] );
    cout << "generate Missed Points (Texture) : frame " << shift
         << ", # of Missed Points Texture : " << frame.getMissedPointsPatch( 0 ).size() << endl;
    // for resizing for mpgeometry
    auto& MPTexFrame = videoMPsTexture.getFrame( shift );
    maxWidth         = ( std::max )( maxWidth, MPTexFrame.getWidth() );
    maxHeight        = ( std::max )( maxHeight, MPTexFrame.getHeight() );
  }
  // resizing for mpgeometry
  assert( maxWidth % 8 == 0 );
  assert( maxHeight % 8 == 0 );
  context.setMPAttWidth( maxWidth );
  context.setMPAttHeight( maxHeight );
  for ( auto& frame : context.getFrames() ) {
    const size_t shift      = frame.getIndex();
    auto&        MPTexFrame = videoMPsTexture.getFrame( shift );
    MPTexFrame.resize( maxWidth, maxHeight );
  }

  cout << "MissedPoints Texture [done]" << endl;
}

void PCCEncoder::generateMPsGeometryImage( PCCContext& context, PCCFrameContext& frame, PCCImageGeometry& image ) {
  size_t width = frame.getMPGeoWidth();

  const int16_t     infiniteDepth = ( std::numeric_limits<int16_t>::max )();
  std::vector<bool> pcmOccupancyMap;
  size_t            pcmOccupancySizeU = width / params_.occupancyResolution_;
  size_t            pcmOccupancySizeV = 1;
  size_t            pcmHeight         = 0;
  size_t            pcmWidth          = width;
  pcmOccupancyMap.resize( pcmOccupancySizeU * pcmOccupancySizeV, false );
  packMissedPointsPatch( frame, pcmOccupancyMap, pcmWidth, pcmHeight, pcmOccupancySizeU, pcmOccupancySizeV, 0 );
  frame.setMPGeoHeight( pcmHeight );
  image.resize( pcmWidth, pcmHeight );
  image.set( 0 );
  uint16_t lastValue{0};
  uint16_t lastY{0};
  uint16_t lastZ{0};
  size_t   numberOfMpsPatches = frame.getNumberOfMissedPointsPatches();
  for ( int i = 0; i < numberOfMpsPatches; i++ ) {
    auto&        missedPointsPatch = frame.getMissedPointsPatch( i );
    const size_t v0                = missedPointsPatch.v0_ * missedPointsPatch.occupancyResolution_;
    const size_t u0                = missedPointsPatch.u0_ * missedPointsPatch.occupancyResolution_;
    size_t       numberOfMps       = missedPointsPatch.getNumberOfMps();
    printf( "generateMPsGeometryImage:: (u0,v0,sizeU,sizeU) = (%zu,%zu,%zu,%zu) \n", u0, v0, missedPointsPatch.sizeU_,
            missedPointsPatch.sizeV_ );
    if ( params_.losslessGeo444_ ) {
      lastValue = missedPointsPatch.x_[numberOfMps - 1];
      lastY     = missedPointsPatch.y_[numberOfMps - 1];
      lastZ     = missedPointsPatch.z_[numberOfMps - 1];
    } else {
      numberOfMps *= 3;
      lastValue = missedPointsPatch.x_[numberOfMps - 1];
    }
    if ( missedPointsPatch.size() ) {
      for ( size_t v = 0; v < missedPointsPatch.sizeV_; ++v ) {
        for ( size_t u = 0; u < missedPointsPatch.sizeU_; ++u ) {
          const size_t p = v * missedPointsPatch.sizeU_ + u;
          if ( p < numberOfMps && missedPointsPatch.x_[p] < infiniteDepth ) {
            const size_t x = ( u0 + u );
            const size_t y = ( v0 + v );
            assert( x < pcmWidth && y < pcmHeight );
            image.setValue( 0, x, y, uint16_t( missedPointsPatch.x_[p] ) );
            if ( params_.losslessGeo444_ ) {
              image.setValue( 1, x, y, uint16_t( missedPointsPatch.y_[p] ) );
              image.setValue( 2, x, y, uint16_t( missedPointsPatch.z_[p] ) );
            }
          } else {
            const size_t x = ( u0 + u );
            const size_t y = ( v0 + v );
            image.setValue( 0, x, y, static_cast<uint16_t>( lastValue ) );
            if ( params_.losslessGeo444_ ) {
              image.setValue( 1, x, y, uint16_t( lastY ) );
              image.setValue( 2, x, y, uint16_t( lastZ ) );
            }
          }
        }  // u
      }    // v
    }      // size()!=0
  }
}

void PCCEncoder::generateMPsTextureImage( PCCContext&         context,
                                          PCCFrameContext&    frame,
                                          PCCImageTexture&    image,
                                          size_t              shift,
                                          const PCCPointSet3& reconstruct ) {
  bool   losslessAtt               = frame.getLosslessGeo();
  size_t numberOfMpsPatches        = frame.getNumberOfMissedPointsPatches();
  size_t numberOfEddPoints         = frame.getTotalNumberOfEddPoints();
  size_t numOfMPGeos               = frame.getTotalNumberOfMissedPoints();
  size_t nPixelInCurrentBlockCount = 0;
  size_t heightEDD                 = 0;
  size_t heightMP                  = 0;
  image.set( 0 );
  double avgR{0.0};
  double avgG{0.0};
  double avgB{0.0};
  size_t xx, yy;
  size_t width = frame.getMPAttWidth();
  if ( numOfMPGeos != 0 ) {
    heightMP         = numOfMPGeos / width + 1;
    size_t heightby8 = heightMP / 8;
    if ( heightby8 * 8 != heightMP ) { heightMP = ( heightby8 + 1 ) * 8; }
    size_t mpsV0;
    size_t maxMpsV0 = 0;
    for ( int i = 0; i < numberOfMpsPatches; i++ ) {
      auto& missedPointsPatch = frame.getMissedPointsPatch( i );
      mpsV0 = missedPointsPatch.v0_ * missedPointsPatch.occupancyResolution_ + missedPointsPatch.sizeV_;
      if ( mpsV0 > maxMpsV0 ) maxMpsV0 = mpsV0;
    }
    heightMP = maxMpsV0;
    image.resize( width, heightMP );
    std::vector<PCCColor3B>& mpsTextures     = frame.getMpsTextures();
    int                      framePointIndex = 0;
    for ( int i = 0; i < numberOfMpsPatches; i++ ) {
      int          pointIndex        = 0;
      auto&        missedPointsPatch = frame.getMissedPointsPatch( i );
      size_t       numMps            = missedPointsPatch.getNumberOfMps();
      const size_t v0                = missedPointsPatch.v0_ * missedPointsPatch.occupancyResolution_;
      const size_t u0                = missedPointsPatch.u0_ * missedPointsPatch.occupancyResolution_;
      for ( size_t v = 0; v < missedPointsPatch.sizeV_; ++v ) {
        for ( size_t u = 0; u < missedPointsPatch.sizeU_; ++u ) {
          //          const size_t p = v * missedPointsPatch.sizeU_ + u;
          const size_t x = ( u0 + u );
          const size_t y = ( v0 + v );
          if ( pointIndex < numMps ) {
            assert( x < width && y < heightMP );
            image.setValue( 0, x, y, uint16_t( mpsTextures[framePointIndex].r() ) );
            image.setValue( 1, x, y, uint16_t( mpsTextures[framePointIndex].g() ) );
            image.setValue( 2, x, y, uint16_t( mpsTextures[framePointIndex].b() ) );
            framePointIndex++;
            pointIndex++;
          }
        }
      }
    }
  }

  if ( numberOfEddPoints != 0 ) {
    size_t eddPatchOffset = 0;
    for ( auto& eddPointsPatch : frame.getEomPatches() ) {
      heightEDD         = eddPointsPatch.eddCount_ / width + 1;
      size_t heightby16 = heightEDD / 16;
      if ( heightby16 * 16 != heightEDD ) { heightEDD = ( heightby16 + 1 ) * 16; }
      eddPointsPatch.u0_    = 0;
      eddPointsPatch.v0_    = heightMP;
      eddPointsPatch.sizeU_ = width;
      eddPointsPatch.sizeV_ = eddPointsPatch.eddCount_ / width;
      image.resize( width, heightMP + heightEDD );
      std::vector<PCCColor3B>& eddTextures = frame.getEddTextures();
      for ( size_t k = 0; k < eddPointsPatch.eddCount_; k++ ) {
        size_t nBlock = k / 256;
        size_t uBlock = nBlock % ( width / 16 );
        size_t vBlock = nBlock / ( width / 16 );
        xx            = uBlock * 16 + ( nPixelInCurrentBlockCount % 16 );
        yy            = vBlock * 16 + ( nPixelInCurrentBlockCount / 16 ) + heightMP;
        ++nPixelInCurrentBlockCount;
        if ( nPixelInCurrentBlockCount >= 256 ) nPixelInCurrentBlockCount = 0;
        assert( yy < heightMP + heightEDD );
        image.setValue( 0, xx, yy, eddTextures[k + eddPatchOffset].r() );
        image.setValue( 1, xx, yy, eddTextures[k + eddPatchOffset].g() );
        image.setValue( 2, xx, yy, eddTextures[k + eddPatchOffset].b() );
        avgR = avgR + double( eddTextures[k + eddPatchOffset].r() ) / eddPointsPatch.eddCount_;
        avgG = avgG + double( eddTextures[k + eddPatchOffset].g() ) / eddPointsPatch.eddCount_;
        avgB = avgB + double( eddTextures[k + eddPatchOffset].b() ) / eddPointsPatch.eddCount_;
      }
      if ( !losslessAtt ) {
        for ( size_t k = numOfMPGeos; k < width * heightMP; ++k ) {
          xx = k % width;
          yy = k / width;
          image.setValue( 0, xx, yy, static_cast<uint8_t>( avgR ) );
          image.setValue( 1, xx, yy, static_cast<uint8_t>( avgG ) );
          image.setValue( 2, xx, yy, static_cast<uint8_t>( avgB ) );
        }
      }
      eddPatchOffset += eddPointsPatch.eddCount_;
    }
  }
}

bool PCCEncoder::generateGeometryVideo( const PCCGroupOfFrames& sources, PCCContext& context ) {
  PCCPatchSegmenter3Parameters params;
  bool                         res             = true;
  auto&                        videoGeometry   = context.getVideoGeometry();
  auto&                        frames          = context.getFrames();
  params.nnNormalEstimation_                   = params_.nnNormalEstimation_;
  params.gridBasedRefineSegmentation_          = params_.gridBasedRefineSegmentation_;
  params.maxNNCountRefineSegmentation_         = params_.maxNNCountRefineSegmentation_;
  params.iterationCountRefineSegmentation_     = params_.iterationCountRefineSegmentation_;
  params.voxelDimensionRefineSegmentation_     = params_.voxelDimensionRefineSegmentation_;
  params.searchRadiusRefineSegmentation_       = params_.searchRadiusRefineSegmentation_;
  params.occupancyResolution_                  = params_.occupancyResolution_;
  params.quantizerSizeX_                       = 1 << context.getLog2PatchQuantizerSizeX();
  params.quantizerSizeY_                       = 1 << context.getLog2PatchQuantizerSizeY();
  params.minPointCountPerCCPatchSegmentation_  = params_.minPointCountPerCCPatchSegmentation_;
  params.maxNNCountPatchSegmentation_          = params_.maxNNCountPatchSegmentation_;
  params.surfaceThickness_                     = params_.surfaceThickness_;
  params.minLevel_                             = params_.minLevel_;
  params.maxAllowedDist2MissedPointsDetection_ = params_.maxAllowedDist2MissedPointsDetection_;
  params.maxAllowedDist2MissedPointsSelection_ = params_.maxAllowedDist2MissedPointsSelection_;
  params.lambdaRefineSegmentation_             = params_.lambdaRefineSegmentation_;
  params.useEnhancedDeltaDepthCode_            = params_.enhancedDeltaDepthCode_;
  params.absoluteD1_                           = params_.absoluteD1_;
  params.surfaceSeparation_                    = params_.surfaceSeparation_;
  params.additionalProjectionPlaneMode_        = params_.additionalProjectionPlaneMode_;
  params.partialAdditionalProjectionPlane_     = params_.partialAdditionalProjectionPlane_;
  params.maxAllowedDepth_                      = ( 1 << params_.geometryNominal2dBitdepth_ ) - 1;
  params.geometryBitDepth3D_                   = params_.geometry3dCoordinatesBitdepth_;
  params.EOMFixBitCount_                       = params_.EOMFixBitCount_;
  params.EOMSingleLayerMode_                   = params_.enhancedDeltaDepthCode_ && ( params_.mapCountMinus1_ == 0 );
  params.patchExpansion_                       = params_.patchExpansion_;
  params.highGradientSeparation_               = params_.highGradientSeparation_;
  params.minGradient_                          = params_.minGradient_;
  params.minNumHighGradientPoints_             = params_.minNumHighGradientPoints_;
  params.enablePointCloudPartitioning_         = params_.enablePointCloudPartitioning_;
  params.roiBoundingBoxMinX_                   = params_.roiBoundingBoxMinX_;
  params.roiBoundingBoxMaxX_                   = params_.roiBoundingBoxMaxX_;
  params.roiBoundingBoxMinY_                   = params_.roiBoundingBoxMinY_;
  params.roiBoundingBoxMaxY_                   = params_.roiBoundingBoxMaxY_;
  params.roiBoundingBoxMinZ_                   = params_.roiBoundingBoxMinZ_;
  params.roiBoundingBoxMaxZ_                   = params_.roiBoundingBoxMaxZ_;
  params.numTilesHor_                          = params_.numTilesHor_;
  params.tileHeightToWidthRatio_               = params_.tileHeightToWidthRatio_;
  params.numCutsAlong1stLongestAxis_           = params_.numCutsAlong1stLongestAxis_;
  params.numCutsAlong2ndLongestAxis_           = params_.numCutsAlong2ndLongestAxis_;
  params.numCutsAlong3rdLongestAxis_           = params_.numCutsAlong3rdLongestAxis_;
  params.createSubPointCloud_ = params_.pointLocalReconstruction_ || params_.singleMapPixelInterleaving_;
  if ( params_.additionalProjectionPlaneMode_ == 0 || params_.additionalProjectionPlaneMode_ == 5 ) {
    calculateWeightNormal( context, sources[0], frames[0] );
    params.weightNormal_ = frames[0].getWeightNormal();
  }
  float sumDistanceSrcRec = 0;
  for ( size_t i = 0; i < frames.size(); i++ ) {
    size_t preIndex       = i > 0 ? ( i - 1 ) : 0;
    float  distanceSrcRec = 0;
    if ( !generateGeometryVideo( sources[i], frames[i], params, videoGeometry, frames[preIndex], i, distanceSrcRec ) ) {
      res = false;
      break;
    }
    sumDistanceSrcRec += distanceSrcRec;
  }
  if ( params_.pointLocalReconstruction_ || params_.singleMapPixelInterleaving_ ) {
    const float distanceSrcRec = sumDistanceSrcRec / (float)frames.size();
    if ( distanceSrcRec >= 250.f ) {
      params_.pointLocalReconstruction_   = false;
      params_.mapCountMinus1_             = 1;
      params_.singleMapPixelInterleaving_ = false;
    }
  }
  if ( params_.pointLocalReconstruction_ ) {
    for ( auto& frame : frames ) {
      for ( auto& patch : frame.getPatches() ) { patch.getOriginalIndex() = patch.getIndex(); }
    }
  }
  return res;
}

void PCCEncoder::pointLocalReconstructionSearch( PCCContext& context, const GeneratePointCloudParameters params ) {
  auto& frames          = context.getFrames();
  auto& videoGeometry   = context.getVideoGeometry();
  auto& videoGeometryD1 = context.getVideoGeometryD1();
  for ( size_t i = 0; i < frames.size(); i++ ) {
    pointLocalReconstructionSearch( context, frames[i], videoGeometry, videoGeometryD1, params );
  }
}

void PCCEncoder::pointLocalReconstructionSearch( PCCContext&                        context,
                                                 PCCFrameContext&                   frame,
                                                 const PCCVideoGeometry&            video,
                                                 const PCCVideoGeometry&            videoD1,
                                                 const GeneratePointCloudParameters params ) {
  auto&                 patches         = frame.getPatches();
  auto&                 blockToPatch    = frame.getBlockToPatch();
  auto&                 occupancyMapOrg = frame.getOccupancyMap();
  std::vector<uint32_t> occupancyMap;
  occupancyMap.resize( occupancyMapOrg.size(), 0 );
  for ( size_t i = 0; i < occupancyMapOrg.size(); i++ ) { occupancyMap[i] = ( occupancyMapOrg[i] >= 1 ); }
  const size_t width              = frame.getWidth();
  const size_t height             = frame.getHeight();
  const size_t blockToPatchWidth  = width / params_.occupancyResolution_;
  const size_t blockToPatchHeight = height / params_.occupancyResolution_;
  const size_t blockSize0         = params_.occupancyResolution_ / params_.occupancyPrecision_;
  for ( size_t v0 = 0; v0 < blockToPatchHeight; ++v0 ) {
    for ( size_t u0 = 0; u0 < blockToPatchWidth; ++u0 ) {
      for ( size_t v1 = 0; v1 < blockSize0; ++v1 ) {
        const size_t v2 = v0 * params_.occupancyResolution_ + v1 * params_.occupancyPrecision_;
        for ( size_t u1 = 0; u1 < blockSize0; ++u1 ) {
          const size_t u2     = u0 * params_.occupancyResolution_ + u1 * params_.occupancyPrecision_;
          bool         isFull = false;
          for ( size_t v3 = 0; v3 < params_.occupancyPrecision_ && !isFull; ++v3 ) {
            for ( size_t u3 = 0; u3 < params_.occupancyPrecision_ && !isFull; ++u3 ) {
              isFull |= occupancyMap[( v2 + v3 ) * width + u2 + u3] == 1;
            }
          }
          for ( size_t v3 = 0; v3 < params_.occupancyPrecision_; ++v3 ) {
            for ( size_t u3 = 0; u3 < params_.occupancyPrecision_; ++u3 ) {
              occupancyMap[( v2 + v3 ) * width + u2 + u3] = isFull;
            }
          }
        }
      }
    }
  }
  size_t shift;
  if ( params.multipleStreams_ ) {
    shift = frame.getIndex();
    if ( video.getFrameCount() < ( shift + 1 ) ) { return; }
  } else {
    shift = frame.getIndex() * ( params.mapCountMinus1_ + 1 );
    if ( video.getFrameCount() < ( shift + ( params.mapCountMinus1_ + 1 ) ) ) { return; }
  }
  const size_t patchCount           = patches.size();
  size_t       nbOfOptimizationMode = context.getPointLocalReconstructionModeNumber();
  const size_t imageWidth           = video.getWidth();
  const size_t imageHeight          = video.getHeight();
  for ( size_t patchIndex = 0; patchIndex < patchCount; ++patchIndex ) {
    const size_t  patchIndexPlusOne = patchIndex + 1;
    auto&         patch             = patches[patchIndex];
    const size_t& patchSize         = patch.getSizeU0() * patch.getSizeV0();
    if ( patchSize == 1 || patchSize <= params_.patchSize_ ) {
      patch.getPointLocalReconstructionLevel()     = 1;
      auto&                     srcPointCloudPatch = frame.getSrcPointCloudByPatch( patch.getOriginalIndex() );
      std::vector<PCCPointSet3> reconstruct;
      std::vector<float>        distance;
      reconstruct.resize( nbOfOptimizationMode );
      distance.resize( nbOfOptimizationMode );
      size_t optimizationIndex = 0, optimizationIndexMin = 0;
      for ( size_t i = 0; i < nbOfOptimizationMode; i++ ) {
        auto& mode = context.getPointLocalReconstructionMode( i );
        for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
          for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
            const size_t blockIndex = patch.patchBlock2CanvasBlock( u0, v0, blockToPatchWidth, blockToPatchHeight );
            if ( blockToPatch[blockIndex] == patchIndexPlusOne ) {
              for ( size_t v1 = 0; v1 < patch.getOccupancyResolution(); ++v1 ) {
                const size_t v = v0 * patch.getOccupancyResolution() + v1;
                for ( size_t u1 = 0; u1 < patch.getOccupancyResolution(); ++u1 ) {
                  const size_t u = u0 * patch.getOccupancyResolution() + u1;
                  size_t       x, y;
                  const bool   occupancy = occupancyMap[patch.patch2Canvas( u, v, imageWidth, imageHeight, x, y )] != 0;
                  if ( !occupancy ) { continue; }
                  auto createdPoints = generatePoints( params, frame, video, videoD1, shift, patchIndex, u, v, x, y,
                                                       mode.interpolate_, mode.filling_, mode.minD1_, mode.neighbor_ );
                  if ( createdPoints.size() > 0 ) {
                    for ( size_t i = 0; i < createdPoints.size(); i++ ) {
                      reconstruct[optimizationIndex].addPoint( createdPoints[i] );
                    }
                  }
                }
              }
            }
          }
        }
        float distancePSrcRec, distancePRecSrc;
        srcPointCloudPatch.distanceGeo( reconstruct[optimizationIndex], distancePSrcRec, distancePRecSrc );
        distance[optimizationIndex] = ( std::max )( distancePSrcRec, distancePRecSrc );
        if ( optimizationIndex == 0 || distance[optimizationIndexMin] > distance[optimizationIndex] ) {
          optimizationIndexMin                    = optimizationIndex;
          patch.getPointLocalReconstructionMode() = optimizationIndexMin;
        }
        optimizationIndex++;
      }
    } else {
      patch.getPointLocalReconstructionLevel() = 0;
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
          patch.getPointLocalReconstructionMode( u0, v0 ) = 0;
          const size_t blockIndex = patch.patchBlock2CanvasBlock( u0, v0, blockToPatchWidth, blockToPatchHeight );
          if ( blockToPatch[blockIndex] == patchIndexPlusOne ) {
            auto&        srcPointCloudPatch = frame.getSrcPointCloudByPatch( patch.getOriginalIndex() );
            PCCPointSet3 blockSrcPointCloud;
            const size_t xMin = u0 * patch.getOccupancyResolution() + patch.getU1();
            const size_t yMin = v0 * patch.getOccupancyResolution() + patch.getV1();
            for ( size_t i = 0; i < srcPointCloudPatch.getPointCount(); i++ ) {
              if ( xMin <= srcPointCloudPatch[i][patch.getTangentAxis()] &&
                   srcPointCloudPatch[i][patch.getTangentAxis()] < xMin + patch.getOccupancyResolution() &&
                   yMin <= srcPointCloudPatch[i][patch.getBitangentAxis()] &&
                   srcPointCloudPatch[i][patch.getBitangentAxis()] < yMin + patch.getOccupancyResolution() ) {
                blockSrcPointCloud.addPoint( srcPointCloudPatch[i] );
              }
            }
            std::vector<PCCPointSet3> reconstruct;
            std::vector<float>        distance;
            reconstruct.resize( nbOfOptimizationMode );
            distance.resize( nbOfOptimizationMode );
            size_t optimizationIndex = 0, optimizationIndexMin = 0;
            for ( size_t i = 0; i < nbOfOptimizationMode; i++ ) {
              auto& mode = context.getPointLocalReconstructionMode( i );
              for ( size_t v1 = 0; v1 < patch.getOccupancyResolution(); ++v1 ) {
                const size_t v = v0 * patch.getOccupancyResolution() + v1;
                for ( size_t u1 = 0; u1 < patch.getOccupancyResolution(); ++u1 ) {
                  const size_t u = u0 * patch.getOccupancyResolution() + u1;
                  size_t       x, y;
                  const bool   occupancy = occupancyMap[patch.patch2Canvas( u, v, imageWidth, imageHeight, x, y )] != 0;
                  if ( !occupancy ) { continue; }
                  auto createdPoints = generatePoints( params, frame, video, videoD1, shift, patchIndex, u, v, x, y,
                                                       mode.interpolate_, mode.filling_, mode.minD1_, mode.neighbor_ );
                  if ( createdPoints.size() > 0 ) {
                    for ( size_t i = 0; i < createdPoints.size(); i++ ) {
                      if ( patch.getAxisOfAdditionalPlane() == 0 ) {
                        reconstruct[optimizationIndex].addPoint( createdPoints[i] );
                      } else {
                        PCCVector3D tmp;
                        PCCPatch::InverseRotatePosition45DegreeOnAxis(
                            patch.getAxisOfAdditionalPlane(), params.geometryBitDepth3D_, createdPoints[i], tmp );
                        reconstruct[optimizationIndex].addPoint( tmp );
                      }
                    }
                  }
                }
              }
              float distancePSrcRec, distancePRecSrc;
              blockSrcPointCloud.distanceGeo( reconstruct[optimizationIndex], distancePSrcRec, distancePRecSrc );
              distance[optimizationIndex] = ( std::max )( distancePSrcRec, distancePRecSrc );
              if ( optimizationIndex == 0 || distance[optimizationIndexMin] > distance[optimizationIndex] ) {
                optimizationIndexMin                            = optimizationIndex;
                patch.getPointLocalReconstructionMode( u0, v0 ) = optimizationIndexMin;
              }
              optimizationIndex++;
            }
          }  // if block is used
        }
      }
    }
  }  // patch
}

bool PCCEncoder::resizeGeometryVideo( PCCContext& context ) {
  size_t maxWidth = 0, maxHeight = 0;
  for ( auto& frame : context.getFrames() ) {
    maxWidth  = ( std::max )( maxWidth, frame.getWidth() );
    maxHeight = ( std::max )( maxHeight, frame.getHeight() );
  }
  maxWidth  = ( std::max )( maxWidth, params_.minimumImageWidth_ );
  maxHeight = ( std::max )( maxHeight, params_.minimumImageHeight_ );
  for ( auto& frame : context.getFrames() ) {
    frame.getWidth()  = maxWidth;
    frame.getHeight() = maxHeight;
    frame.getOccupancyMap().resize( ( maxWidth / params_.occupancyResolution_ ) *
                                    ( maxHeight / params_.occupancyResolution_ ) );
  }
  return true;
}

bool PCCEncoder::dilateGeometryVideo( const PCCGroupOfFrames& sources, PCCContext& context ) {
  auto& videoGeometry     = context.getVideoGeometry();
  auto& videoGeometryD1   = context.getVideoGeometryD1();
  auto& videoOccupancyMap = context.getVideoOccupancyMap();
  auto& frames            = context.getFrames();
  for ( size_t i = 0; i < frames.size(); i++ ) {
    const size_t geometryVideoSize = videoGeometry.getFrameCount();
    if ( params_.multipleStreams_ ) {
      videoGeometry.resize( geometryVideoSize + 1 );
      videoGeometryD1.resize( geometryVideoSize + 1 );
      auto& frame0 = videoGeometry.getFrame( geometryVideoSize );
      generateIntraImage( frames[i], 0, frame0 );
      auto& frame1 = videoGeometryD1.getFrame( geometryVideoSize );
      generateIntraImage( frames[i], 1, frame1 );
      dilate3DPadding( sources[i], frames[i], frame0, videoOccupancyMap.getFrame( i ) );
      if ( !params_.absoluteD1_ ) {
        dilate3DPadding( sources[i], frames[i], frame1, videoOccupancyMap.getFrame( i ) );
      }
    } else {
      const size_t mapCount = params_.mapCountMinus1_ + 1;
      videoGeometry.resize( geometryVideoSize + mapCount );

      if ( params_.singleMapPixelInterleaving_ ) {
        auto& frame1 = videoGeometry.getFrame( geometryVideoSize );
        generateIntraImage( frames[i], 0, frame1 );
        dilate( frames[i], frame1 );
        PCCImageGeometry frame2;
        generateIntraImage( frames[i], 1, frame2 );
        dilate3DPadding( sources[i], frames[i], frame2, videoOccupancyMap.getFrame( i ) );
        for ( size_t x = 0; x < frame1.getWidth(); x++ ) {
          for ( size_t y = 0; y < frame1.getHeight(); y++ ) {
            if ( ( x + y ) % 2 == 1 ) { frame1.setValue( 0, x, y, frame2.getValue( 0, x, y ) ); }
          }
        }
      } else {
        for ( size_t f = 0; f < mapCount; ++f ) {
          auto& frame1 = videoGeometry.getFrame( geometryVideoSize + f );
          generateIntraImage( frames[i], f, frame1 );
          dilate3DPadding( sources[i], frames[i], videoGeometry.getFrame( geometryVideoSize + f ),
                            videoOccupancyMap.getFrame( i ) );
        }
      }
    }
  }
  return true;
}

template <typename T>
void PCCEncoder::dilate( PCCFrameContext& frame, PCCImage<T, 3>& image, const PCCImage<T, 3>* reference ) {
  auto          occupancyMapTemp         = frame.getOccupancyMap();
  const size_t  pixelBlockCount          = params_.occupancyResolution_ * params_.occupancyResolution_;
  const size_t  occupancyMapSizeU        = image.getWidth() / params_.occupancyResolution_;
  const size_t  occupancyMapSizeV        = image.getHeight() / params_.occupancyResolution_;
  const int64_t neighbors[4][2]          = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
  const size_t  MAX_OCCUPANCY_RESOLUTION = 64;
  assert( params_.occupancyResolution_ <= MAX_OCCUPANCY_RESOLUTION );
  size_t              count[MAX_OCCUPANCY_RESOLUTION][MAX_OCCUPANCY_RESOLUTION];
  PCCVector3<int32_t> values[MAX_OCCUPANCY_RESOLUTION][MAX_OCCUPANCY_RESOLUTION];

  for ( size_t v1 = 0; v1 < occupancyMapSizeV; ++v1 ) {
    const int64_t v0 = v1 * params_.occupancyResolution_;
    for ( size_t u1 = 0; u1 < occupancyMapSizeU; ++u1 ) {
      const int64_t u0                = u1 * params_.occupancyResolution_;
      size_t        nonZeroPixelCount = 0;
      for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
        for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
          const int64_t x0 = u0 + u2;
          const int64_t y0 = v0 + v2;
          assert( x0 < int64_t( image.getWidth() ) && y0 < int64_t( image.getHeight() ) );
          const size_t location0 = y0 * image.getWidth() + x0;
          if ( params_.enhancedDeltaDepthCode_ ) {
            nonZeroPixelCount += ( occupancyMapTemp[location0] > 0 );
          } else {
            nonZeroPixelCount += ( occupancyMapTemp[location0] == 1 );
          }
        }
      }
      if ( !nonZeroPixelCount ) {
        if ( reference ) {
          for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
            for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
              const size_t x0 = u0 + u2;
              const size_t y0 = v0 + v2;
              image.setValue( 0, x0, y0, reference->getValue( 0, x0, y0 ) );
              image.setValue( 1, x0, y0, reference->getValue( 1, x0, y0 ) );
              image.setValue( 2, x0, y0, reference->getValue( 2, x0, y0 ) );
            }
          }
        } else if ( u1 > 0 ) {
          for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
            for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
              const size_t x0 = u0 + u2;
              const size_t y0 = v0 + v2;
              assert( x0 > 0 );
              const size_t x1 = x0 - 1;
              image.setValue( 0, x0, y0, image.getValue( 0, x1, y0 ) );
              image.setValue( 1, x0, y0, image.getValue( 1, x1, y0 ) );
              image.setValue( 2, x0, y0, image.getValue( 2, x1, y0 ) );
            }
          }
        } else if ( v1 > 0 ) {
          for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
            for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
              const size_t x0 = u0 + u2;
              const size_t y0 = v0 + v2;
              assert( y0 > 0 );
              const size_t y1 = y0 - 1;
              image.setValue( 0, x0, y0, image.getValue( 0, x0, y1 ) );
              image.setValue( 1, x0, y0, image.getValue( 1, x0, y1 ) );
              image.setValue( 2, x0, y0, image.getValue( 2, x0, y1 ) );
            }
          }
        }
        continue;
      }
      for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
        for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
          values[v2][u2] = 0;
          count[v2][u2]  = 0UL;
        }
      }
      uint32_t iteration = 1;
      while ( nonZeroPixelCount < pixelBlockCount ) {
        for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
          for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
            const int64_t x0 = u0 + u2;
            const int64_t y0 = v0 + v2;
            assert( x0 < int64_t( image.getWidth() ) && y0 < int64_t( image.getHeight() ) );
            const size_t location0 = y0 * image.getWidth() + x0;
            if ( occupancyMapTemp[location0] == iteration ) {
              for ( size_t n = 0; n < 4; ++n ) {
                const int64_t x1        = x0 + neighbors[n][0];
                const int64_t y1        = y0 + neighbors[n][1];
                const size_t  location1 = y1 * image.getWidth() + x1;
                if ( x1 >= u0 && x1 < int64_t( u0 + params_.occupancyResolution_ ) && y1 >= v0 &&
                     y1 < int64_t( v0 + params_.occupancyResolution_ ) && occupancyMapTemp[location1] == 0 ) {
                  const int64_t u3 = u2 + neighbors[n][0];
                  const int64_t v3 = v2 + neighbors[n][1];
                  assert( u3 >= 0 && u3 < int64_t( params_.occupancyResolution_ ) );
                  assert( v3 >= 0 && v3 < int64_t( params_.occupancyResolution_ ) );
                  for ( size_t k = 0; k < 3; ++k ) { values[v3][u3][k] += image.getValue( k, x0, y0 ); }
                  ++count[v3][u3];
                }
              }
            }
          }
        }
        for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
          for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
            if ( count[v2][u2] ) {
              ++nonZeroPixelCount;
              const size_t x0             = u0 + u2;
              const size_t y0             = v0 + v2;
              const size_t location0      = y0 * image.getWidth() + x0;
              const size_t c              = count[v2][u2];
              const size_t c2             = c / 2;
              occupancyMapTemp[location0] = iteration + 1;
              for ( size_t k = 0; k < 3; ++k ) { image.setValue( k, x0, y0, T( ( values[v2][u2][k] + c2 ) / c ) ); }
              values[v2][u2] = 0;
              count[v2][u2]  = 0UL;
            }
          }
        }
        ++iteration;
      }
    }
  }
}

// 3D geometry padding
size_t PCCEncoder::adjustDepth3DPadding( size_t            x,
                                           size_t            y,
                                           uint16_t          mean_val,
                                           PCCImageGeometry& image,
                                           PCCKdTree&        kdtree,
                                           PCCFrameContext&  frame ) {
  auto&  blockToPatch = frame.getBlockToPatch();
  auto&  patches      = frame.getPatches();
  size_t block_addr   = ( y / params_.occupancyResolution_ ) * ( image.getWidth() / params_.occupancyResolution_ ) +
                      x / params_.occupancyResolution_;
  size_t patchIndex = blockToPatch[block_addr];
  auto&  patch      = patches[patchIndex - 1];
  size_t distance   = ( std::numeric_limits<int16_t>::max )();
  // testing the mean value
  PCCNNResult result_mean;
  PCCPoint3D  point_mean = patch.canvasTo3D( x, y, mean_val );
  kdtree.search( point_mean, 1, result_mean );
  const double dist2_mean = result_mean.dist( 0 );
  if ( dist2_mean < distance ) {
    image.setValue( 0, x, y, mean_val );
    image.setValue( 1, x, y, 0 );
    image.setValue( 2, x, y, 0 );
    distance = dist2_mean;
  }
  if ( distance != 0 ) {
    // the mean value does not belong to the point cloud, so let's search for a nearby value and see if it is better
    // than the mean.
    size_t deltadepth = 8;
    if ( mean_val < deltadepth ) { deltadepth = mean_val; }
    if ( mean_val + deltadepth > frame.getMaxDepth() ) { deltadepth = frame.getMaxDepth() - mean_val; }
    for ( uint16_t depth = 1; depth < deltadepth; depth++ ) {
      PCCPoint3D point = patch.canvasTo3D( x, y, mean_val + depth );
      // now find the distance between the point and the original point cloud
      PCCNNResult result;
      kdtree.search( point, 1, result );
      const double dist2 = result.dist( 0 );
      if ( dist2 < distance ) {
        image.setValue( 0, x, y, mean_val + depth );
        image.setValue( 1, x, y, 0 );
        image.setValue( 2, x, y, 0 );
        distance = dist2;
      }
      PCCPoint3D point_neg = patch.canvasTo3D( x, y, mean_val - depth );
      // now find the distance between the point and the original point cloud
      PCCNNResult result_neg;
      kdtree.search( point_neg, 1, result_neg );
      const double dist2_neg = result_neg.dist( 0 );
      if ( dist2_neg < distance ) {
        image.setValue( 0, x, y, mean_val - depth );
        image.setValue( 1, x, y, 0 );
        image.setValue( 2, x, y, 0 );
        distance = dist2_neg;
      }
    }
  }
  return 1;
}

void PCCEncoder::dilate3DPadding( const PCCPointSet3&     source,
                                   PCCFrameContext&        frame,
                                   PCCImageGeometry&       image,
                                   PCCImageOccupancyMap&   occupancyMap,
                                   const PCCImageGeometry* reference ) {
  const size_t  pixelBlockCount          = params_.occupancyResolution_ * params_.occupancyResolution_;
  const size_t  occupancyMapSizeU        = image.getWidth() / params_.occupancyResolution_;
  const size_t  occupancyMapSizeV        = image.getHeight() / params_.occupancyResolution_;
  const int64_t neighbors[4][2]          = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
  const size_t  MAX_OCCUPANCY_RESOLUTION = 64;
  assert( params_.occupancyResolution_ <= MAX_OCCUPANCY_RESOLUTION );
  size_t              count[MAX_OCCUPANCY_RESOLUTION][MAX_OCCUPANCY_RESOLUTION];
  PCCVector3<int32_t> values[MAX_OCCUPANCY_RESOLUTION][MAX_OCCUPANCY_RESOLUTION];

  std::vector<uint32_t> occupancyMapTemp;
  auto&                 occupancyMapOriginal = frame.getOccupancyMap();
  occupancyMapTemp.resize( image.getWidth() * image.getHeight(), 0 );
  PCCKdTree kdtree( source );
  // fill in positions that are added to the sequence, because of occupancyMap video coding
  for ( size_t y_OM = 0; y_OM < occupancyMap.getHeight(); ++y_OM ) {
    for ( size_t x_OM = 0; x_OM < occupancyMap.getWidth(); ++x_OM ) {
      if ( occupancyMap.getValue( 0, x_OM, y_OM ) >= 1 ) {
        // this is an area that has active values, update the temporary occupancy Map struture, and store the mean
        // value in this area
        uint16_t mean_val = 0;
        size_t   count    = 0;
        for ( size_t j = 0; j < params_.occupancyPrecision_; j++ ) {
          size_t y = y_OM * params_.occupancyPrecision_ + j;
          for ( size_t i = 0; i < params_.occupancyPrecision_; i++ ) {
            size_t x = x_OM * params_.occupancyPrecision_ + i;
            if ( occupancyMapOriginal[y * image.getWidth() + x] != 0 ) {
              mean_val += image.getValue( 0, x, y );
              count++;
            }
          }
        }
        mean_val /= count;
        // now fill in the missing positions with depth values searched in 3D space
        for ( size_t j = 0; j < params_.occupancyPrecision_; j++ ) {
          size_t y = y_OM * params_.occupancyPrecision_ + j;
          for ( size_t i = 0; i < params_.occupancyPrecision_; i++ ) {
            size_t x = x_OM * params_.occupancyPrecision_ + i;
            // if depth value is undefined, this position will be added, find the best value
            if ( occupancyMapOriginal[y * image.getWidth() + x] == 0 ) {
              // try to find the best value to approximate this new point to the original point cloud
              // get the patch information
              if ( params_.geometryPadding_ == 1 )
                occupancyMapTemp[y * image.getWidth() + x] =
                    adjustDepth3DPadding( x, y, mean_val, image, kdtree, frame );
              else
                occupancyMapTemp[y * image.getWidth() + x] = 0;
            } else
              occupancyMapTemp[y * image.getWidth() + x] = 1;
          }
        }
      }
    }
  }

  // now continue adding the pixels with the previous dilation approach
  for ( size_t v1 = 0; v1 < occupancyMapSizeV; ++v1 ) {
    const int64_t v0 = v1 * params_.occupancyResolution_;
    for ( size_t u1 = 0; u1 < occupancyMapSizeU; ++u1 ) {
      const int64_t u0                = u1 * params_.occupancyResolution_;
      size_t        nonZeroPixelCount = 0;
      for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
        for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
          const int64_t x0 = u0 + u2;
          const int64_t y0 = v0 + v2;
          assert( x0 < int64_t( image.getWidth() ) && y0 < int64_t( image.getHeight() ) );
          const size_t location0 = y0 * image.getWidth() + x0;
          if ( params_.enhancedDeltaDepthCode_ ) {
            nonZeroPixelCount += ( occupancyMapTemp[location0] > 0 );
          } else {
            nonZeroPixelCount += ( occupancyMapTemp[location0] == 1 );
          }
        }
      }
      if ( !nonZeroPixelCount ) {
        if ( reference ) {
          for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
            for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
              const size_t x0 = u0 + u2;
              const size_t y0 = v0 + v2;
              image.setValue( 0, x0, y0, reference->getValue( 0, x0, y0 ) );
              image.setValue( 1, x0, y0, reference->getValue( 1, x0, y0 ) );
              image.setValue( 2, x0, y0, reference->getValue( 2, x0, y0 ) );
            }
          }
        } else if ( u1 > 0 ) {
          for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
            for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
              const size_t x0 = u0 + u2;
              const size_t y0 = v0 + v2;
              assert( x0 > 0 );
              const size_t x1 = x0 - 1;
              image.setValue( 0, x0, y0, image.getValue( 0, x1, y0 ) );
              image.setValue( 1, x0, y0, image.getValue( 1, x1, y0 ) );
              image.setValue( 2, x0, y0, image.getValue( 2, x1, y0 ) );
            }
          }
        } else if ( v1 > 0 ) {
          for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
            for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
              const size_t x0 = u0 + u2;
              const size_t y0 = v0 + v2;
              assert( y0 > 0 );
              const size_t y1 = y0 - 1;
              image.setValue( 0, x0, y0, image.getValue( 0, x0, y1 ) );
              image.setValue( 1, x0, y0, image.getValue( 1, x0, y1 ) );
              image.setValue( 2, x0, y0, image.getValue( 2, x0, y1 ) );
            }
          }
        }
        continue;
      }
      for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
        for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
          values[v2][u2] = 0;
          count[v2][u2]  = 0UL;
        }
      }
      uint32_t iteration = 1;
      while ( nonZeroPixelCount < pixelBlockCount ) {
        for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
          for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
            const int64_t x0 = u0 + u2;
            const int64_t y0 = v0 + v2;
            assert( x0 < int64_t( image.getWidth() ) && y0 < int64_t( image.getHeight() ) );
            const size_t location0 = y0 * image.getWidth() + x0;
            if ( occupancyMapTemp[location0] == iteration ) {
              for ( size_t n = 0; n < 4; ++n ) {
                const int64_t x1        = x0 + neighbors[n][0];
                const int64_t y1        = y0 + neighbors[n][1];
                const size_t  location1 = y1 * image.getWidth() + x1;
                if ( x1 >= u0 && x1 < int64_t( u0 + params_.occupancyResolution_ ) && y1 >= v0 &&
                     y1 < int64_t( v0 + params_.occupancyResolution_ ) && occupancyMapTemp[location1] == 0 ) {
                  const int64_t u3 = u2 + neighbors[n][0];
                  const int64_t v3 = v2 + neighbors[n][1];
                  assert( u3 >= 0 && u3 < int64_t( params_.occupancyResolution_ ) );
                  assert( v3 >= 0 && v3 < int64_t( params_.occupancyResolution_ ) );
                  for ( size_t k = 0; k < 3; ++k ) { values[v3][u3][k] += image.getValue( k, x0, y0 ); }
                  ++count[v3][u3];
                }
              }
            }
          }
        }
        for ( size_t v2 = 0; v2 < params_.occupancyResolution_; ++v2 ) {
          for ( size_t u2 = 0; u2 < params_.occupancyResolution_; ++u2 ) {
            if ( count[v2][u2] ) {
              ++nonZeroPixelCount;
              const size_t x0             = u0 + u2;
              const size_t y0             = v0 + v2;
              const size_t location0      = y0 * image.getWidth() + x0;
              const size_t c              = count[v2][u2];
              const size_t c2             = c / 2;
              occupancyMapTemp[location0] = iteration + 1;
              for ( size_t k = 0; k < 3; ++k ) {
                image.setValue( k, x0, y0, uint16_t( ( values[v2][u2][k] + c2 ) / c ) );
              }
              values[v2][u2] = 0;
              count[v2][u2]  = 0UL;
            }
          }
        }
        ++iteration;
      }
    }
  }
}

/* harmonic background filling algorithm */
// interpolate using 5-point laplacian inpainting
template <typename T>
void PCCEncoder::dilateHarmonicBackgroundFill( PCCFrameContext& frame, PCCImage<T, 3>& image ) {
  auto                               occupancyMapTemp = frame.getOccupancyMap();
  int                                i                = 0;
  std::vector<PCCImage<T, 3>>        mipVec;
  std::vector<std::vector<uint32_t>> mipOccupancyMapVec;
  int                                miplev = 0;

  // create coarse image by dyadic sampling
  while ( 1 ) {
    mipVec.resize( mipVec.size() + 1 );
    mipOccupancyMapVec.resize( mipOccupancyMapVec.size() + 1 );
    if ( miplev > 0 )
      CreateCoarseLayer( mipVec[miplev - 1], mipVec[miplev], mipOccupancyMapVec[miplev - 1],
                         mipOccupancyMapVec[miplev] );
    else
      CreateCoarseLayer( image, mipVec[miplev], occupancyMapTemp, mipOccupancyMapVec[miplev] );

    if ( mipVec[miplev].getWidth() <= 4 || mipVec[miplev].getHeight() <= 4 ) break;
    ++miplev;
  }
  miplev++;
  // push phase: inpaint laplacian
  regionFill( mipVec[miplev - 1], mipOccupancyMapVec[miplev - 1], mipVec[miplev - 1] );
  for ( i = miplev - 1; i >= 0; --i ) {
    if ( i > 0 ) {
      regionFill( mipVec[i - 1], mipOccupancyMapVec[i - 1], mipVec[i] );
    } else {
      regionFill( image, occupancyMapTemp, mipVec[i] );
    }
  }
}

template <typename T>
void PCCEncoder::CreateCoarseLayer( PCCImage<T, 3>&        image,
                                    PCCImage<T, 3>&        mip,
                                    std::vector<uint32_t>& occupancyMap,
                                    std::vector<uint32_t>& mipOccupancyMap ) {
  int dyadicWidth = 1;
  while ( dyadicWidth < image.getWidth() ) dyadicWidth *= 2;
  int dyadicHeight = 1;
  while ( dyadicHeight < image.getHeight() ) dyadicHeight *= 2;
  // allocate the mipmap with half the resolution
  mip.resize( ( dyadicWidth / 2 ), ( dyadicHeight / 2 ) );
  mipOccupancyMap.resize( ( dyadicWidth / 2 ) * ( dyadicHeight / 2 ), 0 );
  int    stride    = image.getWidth();
  int    newStride = ( dyadicWidth / 2 );
  int    x, y, i, j;
  double num[3], den;
  for ( y = 0; y < mip.getHeight(); y++ ) {
    for ( x = 0; x < mip.getWidth(); x++ ) {
      num[0] = 0;
      num[1] = 0;
      num[2] = 0;
      den    = 0;
      for ( i = 0; i < 2; i++ ) {
        for ( j = 0; j < 2; j++ ) {
          int row = ( 2 * y + i ) < 0 ? 0 : ( 2 * y + i ) >= image.getHeight() ? image.getHeight() - 1 : ( 2 * y + i );
          int column = ( 2 * x + j ) < 0 ? 0 : ( 2 * x + j ) >= image.getWidth() ? image.getWidth() - 1 : ( 2 * x + j );
          if ( occupancyMap[column + stride * row] == 1 ) {
            den++;
            for ( int cc = 0; cc < 3; cc++ ) { num[cc] += image.getValue( cc, column, row ); }
          }
        }
      }
      if ( den > 0 ) {
        mipOccupancyMap[x + newStride * y] = 1;
        for ( int cc = 0; cc < 3; cc++ ) { mip.setValue( cc, x, y, std::round( num[cc] / den ) ); }
      }
    }
  }
}

template <typename T>
void PCCEncoder::regionFill( PCCImage<T, 3>& image, std::vector<uint32_t>& occupancyMap, PCCImage<T, 3>& imageLowRes ) {
  int                   stride        = image.getWidth();
  int                   numElem       = 0;
  int                   numSparseElem = 0;
  std::vector<uint32_t> indexing;
  indexing.resize( occupancyMap.size() );
  for ( int i = 0; i < occupancyMap.size(); i++ ) {
    if ( occupancyMap[i] == 0 ) {
      indexing[i] = numElem;
      numElem++;
    }
  }
  // create a sparse matrix with the coefficients
  std::vector<uint32_t> iSparse;
  std::vector<uint32_t> jSparse;
  std::vector<double>   valSparse;
  iSparse.resize( numElem * 5 );
  jSparse.resize( numElem * 5 );
  valSparse.resize( numElem * 5 );
  // create an initial solution using the low-resolution
  std::vector<double> b[3];
  b[0].resize( numElem );
  b[1].resize( numElem );
  b[2].resize( numElem );
  // fill in the system
  int idx       = 0;
  int idxSparse = 0;
  for ( int row = 0; row < image.getHeight(); row++ ) {
    for ( int column = 0; column < image.getWidth(); column++ ) {
      if ( occupancyMap[column + stride * row] == 0 ) {
        int count = 0;
        b[0][idx] = 0;
        b[1][idx] = 0;
        b[2][idx] = 0;
        for ( int i = -1; i < 2; i++ ) {
          for ( int j = -1; j < 2; j++ ) {
            if ( ( i == j ) || ( i == -j ) ) continue;
            if ( ( column + j < 0 ) || ( column + j > image.getWidth() - 1 ) ) continue;
            if ( ( row + i < 0 ) || ( row + i > image.getHeight() - 1 ) ) continue;
            count++;
            if ( occupancyMap[column + j + stride * ( row + i )] == 1 ) {
              b[0][idx] += image.getValue( 0, column + j, row + i );
              b[1][idx] += image.getValue( 1, column + j, row + i );
              b[2][idx] += image.getValue( 2, column + j, row + i );
            } else {
              iSparse[idxSparse]   = idx;
              jSparse[idxSparse]   = indexing[column + j + stride * ( row + i )];
              valSparse[idxSparse] = -1;
              idxSparse++;
            }
          }
        }
        // now insert the weight of the center pixel
        iSparse[idxSparse]   = idx;
        jSparse[idxSparse]   = idx;
        valSparse[idxSparse] = count;
        idx++;
        idxSparse++;
      }
    }
  }
  numSparseElem = idxSparse;
  // now solve the linear system Ax=b using Gauss-Siedel relaxation, with initial guess coming from the lower
  // resolution
  std::vector<double> x[3];
  x[0].resize( numElem );
  x[1].resize( numElem );
  x[2].resize( numElem );
  if ( imageLowRes.getWidth() == image.getWidth() ) {
    // low resolution image not provided, let's use for the initialization the mean value of the active pixels
    double mean[3] = {0.0, 0.0, 0.0};
    idx            = 0;
    for ( int row = 0; row < image.getHeight(); row++ ) {
      for ( int column = 0; column < image.getWidth(); column++ ) {
        if ( occupancyMap[column + stride * row] == 1 ) {
          mean[0] += double( image.getValue( 0, column, row ) );
          mean[1] += double( image.getValue( 1, column, row ) );
          mean[2] += double( image.getValue( 2, column, row ) );
          idx++;
        }
      }
    }
    mean[0] /= idx;
    mean[1] /= idx;
    mean[2] /= idx;
    idx = 0;
    for ( int row = 0; row < image.getHeight(); row++ ) {
      for ( int column = 0; column < image.getWidth(); column++ ) {
        if ( occupancyMap[column + stride * row] == 0 ) {
          x[0][idx] = mean[0];
          x[1][idx] = mean[1];
          x[2][idx] = mean[2];
          idx++;
        }
      }
    }
  } else {
    idx = 0;
    for ( int row = 0; row < image.getHeight(); row++ ) {
      for ( int column = 0; column < image.getWidth(); column++ ) {
        if ( occupancyMap[column + stride * row] == 0 ) {
          x[0][idx] = imageLowRes.getValue( 0, column / 2, row / 2 );
          x[1][idx] = imageLowRes.getValue( 1, column / 2, row / 2 );
          x[2][idx] = imageLowRes.getValue( 2, column / 2, row / 2 );
          idx++;
        }
      }
    }
  }
  int    maxIteration = 1024;
  double maxError     = 0.00001;
  for ( int cc = 0; cc < 3; cc++ ) {
    int it = 0;
    for ( ; it < maxIteration; it++ ) {
      int    idxSparse = 0;
      double error     = 0;
      double val       = 0;
      for ( int centerIdx = 0; centerIdx < numElem; centerIdx++ ) {
        // add the b result
        val = b[cc][centerIdx];
        while ( ( idxSparse < numSparseElem ) && ( iSparse[idxSparse] == centerIdx ) ) {
          if ( valSparse[idxSparse] < 0 ) {
            val += x[cc][jSparse[idxSparse]];
            idxSparse++;
          } else {
            // final value
            val /= valSparse[idxSparse];
            // accumulate the error
            error += ( val - x[cc][centerIdx] ) * ( val - x[cc][centerIdx] );
            // update the value
            x[cc][centerIdx] = val;
            idxSparse++;
          }
        }
      }
      error = error / numElem;
      if ( error < maxError ) { break; }
    }
  }
  // put the value back in the image
  idx = 0;
  for ( int row = 0; row < image.getHeight(); row++ ) {
    for ( int column = 0; column < image.getWidth(); column++ ) {
      if ( occupancyMap[column + stride * row] == 0 ) {
        image.setValue( 0, column, row, x[0][idx] );
        image.setValue( 1, column, row, x[1][idx] );
        image.setValue( 2, column, row, x[2][idx] );
        idx++;
      }
    }
  }
}

/* pull push filling algorithm */
template <typename T>
int PCCEncoder::mean4w( T             p1,
                        unsigned char w1,
                        T             p2,
                        unsigned char w2,
                        T             p3,
                        unsigned char w3,
                        T             p4,
                        unsigned char w4 ) {
  int result = ( p1 * int( w1 ) + p2 * int( w2 ) + p3 * int( w3 ) + p4 * int( w4 ) ) /
               ( int( w1 ) + int( w2 ) + int( w3 ) + int( w4 ) );
  return result;
}

// Generates a weighted mipmap
template <typename T>
void PCCEncoder::pushPullMip( const PCCImage<T, 3>&        image,
                              PCCImage<T, 3>&              mip,
                              const std::vector<uint32_t>& occupancyMap,
                              std::vector<uint32_t>&       mipOccupancyMap ) {
  unsigned char w1, w2, w3, w4;
  unsigned char val1, val2, val3, val4;
  const size_t  width     = image.getWidth();
  const size_t  height    = image.getHeight();
  const size_t  newWidth  = ( ( width + 1 ) >> 1 );
  const size_t  newHeight = ( ( height + 1 ) >> 1 );
  // allocate the mipmap with half the resolution
  mip.resize( newWidth, newHeight );
  mipOccupancyMap.resize( newWidth * newHeight, 0 );
  for ( size_t y = 0; y < newHeight; ++y ) {
    const size_t yUp = y << 1;
    for ( size_t x = 0; x < newWidth; ++x ) {
      const size_t xUp = x << 1;
      if ( occupancyMap[xUp + width * yUp] == 0 )
        w1 = 0;
      else
        w1 = 255;
      if ( ( xUp + 1 >= width ) || ( occupancyMap[xUp + 1 + width * yUp] == 0 ) )
        w2 = 0;
      else
        w2 = 255;
      if ( ( yUp + 1 >= height ) || ( occupancyMap[xUp + width * ( yUp + 1 )] == 0 ) )
        w3 = 0;
      else
        w3 = 255;
      if ( ( xUp + 1 >= width ) || ( yUp + 1 >= height ) || ( occupancyMap[xUp + 1 + width * ( yUp + 1 )] == 0 ) )
        w4 = 0;
      else
        w4 = 255;
      if ( w1 + w2 + w3 + w4 > 0 ) {
        for ( int cc = 0; cc < 3; cc++ ) {
          val1 = image.getValue( cc, xUp, yUp );
          if ( xUp + 1 >= width )
            val2 = 0;
          else
            val2 = image.getValue( cc, xUp + 1, yUp );
          if ( yUp + 1 >= height )
            val3 = 0;
          else
            val3 = image.getValue( cc, xUp, yUp + 1 );
          if ( ( xUp + 1 >= width ) || ( yUp + 1 >= height ) )
            val4 = 0;
          else
            val4 = image.getValue( cc, xUp + 1, yUp + 1 );
          T newVal = mean4w( val1, w1, val2, w2, val3, w3, val4, w4 );
          mip.setValue( cc, x, y, newVal );
        }
        mipOccupancyMap[x + newWidth * y] = 1;
      }
    }
  }
}

// interpolate using mipmap
template <typename T>
void PCCEncoder::pushPullFill( PCCImage<T, 3>&              image,
                               const PCCImage<T, 3>&        mip,
                               const std::vector<uint32_t>& occupancyMap,
                               int                          numIters ) {
  const size_t width    = mip.getWidth();
  const size_t height   = mip.getHeight();
  const size_t widthUp  = image.getWidth();
  const size_t heightUp = image.getHeight();
  assert( ( ( widthUp + 1 ) >> 1 ) == width );
  assert( ( ( heightUp + 1 ) >> 1 ) == height );
  int           x, y, xUp, yUp;
  unsigned char w1, w2, w3, w4;
  for ( yUp = 0; yUp < heightUp; ++yUp ) {
    y = yUp >> 1;
    for ( xUp = 0; xUp < widthUp; ++xUp ) {
      x = xUp >> 1;
      if ( occupancyMap[xUp + widthUp * yUp] == 0 ) {
        if ( ( xUp % 2 == 0 ) && ( yUp % 2 == 0 ) ) {
          w1 = 144;
          w2 = ( x > 0 ? (unsigned char)48 : 0 );
          w3 = ( y > 0 ? (unsigned char)48 : 0 );
          w4 = ( ( ( x > 0 ) && ( y > 0 ) ) ? (unsigned char)16 : 0 );
          for ( int cc = 0; cc < 3; cc++ ) {
            T val       = mip.getValue( cc, x, y );
            T valLeft   = ( x > 0 ? mip.getValue( cc, x - 1, y ) : 0 );
            T valUp     = ( y > 0 ? mip.getValue( cc, x, y - 1 ) : 0 );
            T valUpLeft = ( ( x > 0 && y > 0 ) ? mip.getValue( cc, x - 1, y - 1 ) : 0 );
            T newVal    = mean4w( val, w1, valLeft, w2, valUp, w3, valUpLeft, w4 );
            image.setValue( cc, xUp, yUp, newVal );
          }
        } else if ( ( xUp % 2 == 1 ) && ( yUp % 2 == 0 ) ) {
          w1 = 144;
          w2 = ( x < width - 1 ? (unsigned char)48 : 0 );
          w3 = ( y > 0 ? (unsigned char)48 : 0 );
          w4 = ( ( ( x < width - 1 ) && ( y > 0 ) ) ? (unsigned char)16 : 0 );
          for ( int cc = 0; cc < 3; cc++ ) {
            T val        = mip.getValue( cc, x, y );
            T valRight   = ( x < width - 1 ? mip.getValue( cc, x + 1, y ) : 0 );
            T valUp      = ( y > 0 ? mip.getValue( cc, x, y - 1 ) : 0 );
            T valUpRight = ( ( ( x < width - 1 ) && ( y > 0 ) ) ? mip.getValue( cc, x + 1, y - 1 ) : 0 );
            T newVal     = mean4w( val, w1, valRight, w2, valUp, w3, valUpRight, w4 );
            image.setValue( cc, xUp, yUp, newVal );
          }
        } else if ( ( xUp % 2 == 0 ) && ( yUp % 2 == 1 ) ) {
          w1 = 144;
          w2 = ( x > 0 ? (unsigned char)48 : 0 );
          w3 = ( y < height - 1 ? (unsigned char)48 : 0 );
          w4 = ( ( ( x > 0 ) && ( y < height - 1 ) ) ? (unsigned char)16 : 0 );
          for ( int cc = 0; cc < 3; cc++ ) {
            T val         = mip.getValue( cc, x, y );
            T valLeft     = ( x > 0 ? mip.getValue( cc, x - 1, y ) : 0 );
            T valDown     = ( ( y < height - 1 ) ? mip.getValue( cc, x, y + 1 ) : 0 );
            T valDownLeft = ( ( x > 0 && ( y < height - 1 ) ) ? mip.getValue( cc, x - 1, y + 1 ) : 0 );
            T newVal      = mean4w( val, w1, valLeft, w2, valDown, w3, valDownLeft, w4 );
            image.setValue( cc, xUp, yUp, newVal );
          }
        } else {
          w1 = 144;
          w2 = ( x < width - 1 ? (unsigned char)48 : 0 );
          w3 = ( y < height - 1 ? (unsigned char)48 : 0 );
          w4 = ( ( ( x < width - 1 ) && ( y < height - 1 ) ) ? (unsigned char)16 : 0 );
          for ( int cc = 0; cc < 3; cc++ ) {
            T val          = mip.getValue( cc, x, y );
            T valRight     = ( x < width - 1 ? mip.getValue( cc, x + 1, y ) : 0 );
            T valDown      = ( ( y < height - 1 ) ? mip.getValue( cc, x, y + 1 ) : 0 );
            T valDownRight = ( ( ( x < width - 1 ) && ( y < height - 1 ) ) ? mip.getValue( cc, x + 1, y + 1 ) : 0 );
            T newVal       = mean4w( val, w1, valRight, w2, valDown, w3, valDownRight, w4 );
            image.setValue( cc, xUp, yUp, newVal );
          }
        }
      }
    }
  }
  auto tmpImage( image );
  for ( size_t n = 0; n < numIters; n++ ) {
    for ( int y = 0; y < heightUp; y++ ) {
      for ( int x = 0; x < widthUp; x++ ) {
        if ( occupancyMap[x + widthUp * y] == 0 ) {
          int x1 = ( x > 0 ) ? x - 1 : x;
          int y1 = ( y > 0 ) ? y - 1 : y;
          int x2 = ( x < widthUp - 1 ) ? x + 1 : x;
          int y2 = ( y < heightUp - 1 ) ? y + 1 : y;
          for ( size_t c = 0; c < 3; c++ ) {
            int val = image.getValue( c, x1, y1 ) + image.getValue( c, x2, y1 ) + image.getValue( c, x1, y2 ) +
                      image.getValue( c, x2, y2 ) + image.getValue( c, x1, y ) + image.getValue( c, x2, y ) +
                      image.getValue( c, x, y1 ) + image.getValue( c, x, y2 );
            tmpImage.setValue( c, x, y, ( val + 4 ) >> 3 );
          }
        }
      }
    }
    swap( image, tmpImage );
  }
}

template <typename T>
void PCCEncoder::dilateSmoothedPushPull( PCCFrameContext& frame, PCCImage<T, 3>& image ) {
  auto                               occupancyMapTemp = frame.getOccupancyMap();
  int                                i                = 0;
  std::vector<PCCImage<T, 3>>        mipVec;
  std::vector<std::vector<uint32_t>> mipOccupancyMapVec;
  int                                div    = 2;
  int                                miplev = 0;

  // pull phase create the mipmap
  while ( 1 ) {
    mipVec.resize( mipVec.size() + 1 );
    mipOccupancyMapVec.resize( mipOccupancyMapVec.size() + 1 );
    div *= 2;
    if ( miplev > 0 ) {
      pushPullMip( mipVec[miplev - 1], mipVec[miplev], mipOccupancyMapVec[miplev - 1], mipOccupancyMapVec[miplev] );
    } else {
      pushPullMip( image, mipVec[miplev], occupancyMapTemp, mipOccupancyMapVec[miplev] );
    }
    if ( mipVec[miplev].getWidth() <= 4 || mipVec[miplev].getHeight() <= 4 ) { break; }
    ++miplev;
  }
  miplev++;
#if DEBUG_PATCH
  for ( int k = 0; k < miplev; k++ ) {
    char buf[100];
    sprintf( buf, "mip%02i.rgb", k );
    std::string filename = addVideoFormat( buf, mipVec[k].getWidth(), mipVec[k].getHeight(), false );
    mipVec[k].write( filename, 1 );
  }
#endif
  // push phase: refill
  int numIters = 4;
  for ( i = miplev - 1; i >= 0; --i ) {
    if ( i > 0 ) {
      pushPullFill( mipVec[i - 1], mipVec[i], mipOccupancyMapVec[i - 1], numIters );
    } else {
      pushPullFill( image, mipVec[i], occupancyMapTemp, numIters );
    }
    numIters = ( std::min )( numIters + 1, 16 );
  }
#if DEBUG_PATCH
  for ( int k = 0; k < miplev; k++ ) {
    char buf[100];
    sprintf( buf, "mipfill%02i.rgb", k );
    std::string filename = addVideoFormat( buf, mipVec[k].getWidth(), mipVec[k].getHeight(), false );
    mipVec[k].write( filename, 1 );
  }
#endif
}

void PCCEncoder::presmoothPointCloudColor( PCCPointSet3& reconstruct, const PCCEncoderParameters params ) {
  const size_t            pointCount = reconstruct.getPointCount();
  PCCKdTree               kdtree( reconstruct );
  PCCNNResult             result;
  std::vector<PCCColor3B> temp;
  temp.resize( pointCount );
  for ( size_t m = 0; m < pointCount; ++m ) { temp[m] = reconstruct.getColor( m ); }
  tbb::task_arena limited( (int)params.nbThread_ );
  limited.execute( [&] {
    tbb::parallel_for( size_t( 0 ), pointCount, [&]( const size_t i ) {
      //  for (size_t i = 0; i < pointCount; ++i) {

      PCCNNResult result;
      if ( reconstruct.getBoundaryPointType( i ) == 2 ) {
        kdtree.searchRadius( reconstruct[i], params.neighborCountColorSmoothing_, params.radius2ColorSmoothing_,
                             result );
        PCCVector3D          centroid( 0.0 );
        size_t               neighborCount = 0;
        std::vector<uint8_t> Lum;
        for ( size_t r = 0; r < result.count(); ++r ) {
          const double dist2 = result.dist( r );
          if ( dist2 > params.radius2ColorSmoothing_ ) { break; }
          ++neighborCount;
          const size_t index = result.indices( r );
          PCCColor3B   color = reconstruct.getColor( index );
          centroid[0] += double( color[0] );
          centroid[1] += double( color[1] );
          centroid[2] += double( color[2] );

          double Y = 0.2126 * double( color[0] ) + 0.7152 * double( color[1] ) + 0.0722 * double( color[2] );
          Lum.push_back( uint8_t( Y ) );
        }

        PCCColor3B color;
        if ( neighborCount ) {
          for ( size_t k = 0; k < 3; ++k ) {
            centroid[k] = double( int64_t( centroid[k] + ( neighborCount / 2 ) ) / neighborCount );
          }

          // Texture characterization
          double     H               = entropy( Lum, int( neighborCount ) );
          PCCColor3B colorQP         = reconstruct.getColor( i );
          double     distToCentroid2 = 0;
          for ( size_t k = 0; k < 3; ++k ) { distToCentroid2 += abs( centroid[k] - double( colorQP[k] ) ); }
          if ( distToCentroid2 >= double( params.thresholdColorSmoothing_ ) &&
               H < double( params.thresholdLocalEntropy_ ) ) {
            color[0] = uint8_t( centroid[0] );
            color[1] = uint8_t( centroid[1] );
            color[2] = uint8_t( centroid[2] );
            temp[i]  = color;
          }
        }
      }
    } );
  } );

  limited.execute( [&] {
    tbb::parallel_for( size_t( 0 ), pointCount, [&]( const size_t i ) {
      // for (size_t i = 0; i < pointCount; ++i) {
      reconstruct.setColor( i, temp[i] );
    } );
  } );
}

bool PCCEncoder::generateTextureVideo( const PCCGroupOfFrames&    sources,
                                       PCCGroupOfFrames&          reconstructs,
                                       PCCContext&                context,
                                       const PCCEncoderParameters params ) {  
  auto& frames = context.getFrames();
  bool  ret    = true;
  for ( size_t i = 0; i < frames.size(); i++ ) {
    auto& frame = frames[i];
    size_t mapCount = params_.mapCountMinus1_ + 1;
    if ( params_.pointLocalReconstruction_ ) {
      // create sub reconstruct point cloud
      PCCPointSet3   subReconstruct;
      vector<size_t> subReconstructIndex;
      subReconstructIndex.clear();
      if ( reconstructs[i].hasColors() ) { subReconstruct.addColors(); }
      size_t numPointSub = 0, numPoint = reconstructs[i].getPointCount();
      auto&  pointToPixel = frame.getPointToPixel();
      for ( size_t j = 0; j < numPoint; j++ ) {
        if ( pointToPixel[j][2] < mapCount ) {
          numPointSub++;
          subReconstruct.addPoint( reconstructs[i][j] );
          subReconstructIndex.push_back( j );
        }
      }
      subReconstruct.resize( numPointSub );
      sources[i].transferColors( subReconstruct, int32_t( params_.bestColorSearchRange_ ), params_.losslessGeo_ == 1,
                                 params_.numNeighborsColorTransferFwd_, params_.numNeighborsColorTransferBwd_,
                                 params_.useDistWeightedAverageFwd_, params_.useDistWeightedAverageBwd_,
                                 params_.skipAvgIfIdenticalSourcePointPresentFwd_,
                                 params_.skipAvgIfIdenticalSourcePointPresentBwd_, params_.distOffsetFwd_,
                                 params_.distOffsetBwd_, params_.maxGeometryDist2Fwd_, params_.maxGeometryDist2Bwd_,
                                 params_.maxColorDist2Fwd_, params_.maxColorDist2Bwd_, params_.excludeColorOutlier_,
                                 params_.thresholdColorOutlierDist_ );

      for ( size_t j = 0; j < numPointSub; j++ ) {
        reconstructs[i].setColor( subReconstructIndex[j], subReconstruct.getColor( j ) );
        subReconstruct.setBoundaryPointType( j, reconstructs[i].getBoundaryPointType( subReconstructIndex[j] ) );
      }
      // color pre-smoothing
      if ( !params_.losslessGeo_ && params_.flagColorPreSmoothing_ ) {
        presmoothPointCloudColor( subReconstruct, params );
        for ( size_t j = 0; j < numPointSub; j++ ) {
          reconstructs[i].setColor( subReconstructIndex[j], subReconstruct.getColor( j ) );
        }
      }
    } else {
      sources[i].transferColors( reconstructs[i], int32_t( params_.bestColorSearchRange_ ), params_.losslessGeo_ == 1,
                                 params_.numNeighborsColorTransferFwd_, params_.numNeighborsColorTransferBwd_,
                                 params_.useDistWeightedAverageFwd_, params_.useDistWeightedAverageBwd_,
                                 params_.skipAvgIfIdenticalSourcePointPresentFwd_,
                                 params_.skipAvgIfIdenticalSourcePointPresentBwd_, params_.distOffsetFwd_,
                                 params_.distOffsetBwd_, params_.maxGeometryDist2Fwd_, params_.maxGeometryDist2Bwd_,
                                 params_.maxColorDist2Fwd_, params_.maxColorDist2Bwd_, params_.excludeColorOutlier_,
                                 params_.thresholdColorOutlierDist_ );
      // color pre-smoothing
      if ( !params_.losslessGeo_ && params_.flagColorPreSmoothing_ ) {
        presmoothPointCloudColor( reconstructs[i], params );
      }
    }
    ret &= generateTextureVideo( reconstructs[i], context, i, mapCount );
  }
  return ret;
}

bool PCCEncoder::generateTextureVideo( const PCCPointSet3& reconstruct,
                                       PCCContext&         context,
                                       size_t              frameIndex,
                                       const size_t        mapCount ) {
  auto& frame   = context[frameIndex];
  auto& video   = context.getVideoTexture();
  auto& videoT1 = context.getVideoTextureT1();
  assert( mapCount == 2 );
  auto&  pointToPixel                 = frame.getPointToPixel();
  bool   useMissedPointsSeparateVideo = frame.getUseMissedPointsSeparateVideo();
  bool   losslessAtt                  = frame.getLosslessGeo();
  bool   losslessGeo                  = frame.getLosslessGeo();
  bool   lossyMissedPointsPatch       = frame.getRawPatchEnabledFlag() && ( !losslessGeo );
  size_t numberOfEddPoints            = frame.getTotalNumberOfEddPoints();
  size_t numOfMPGeos                  = frame.getTotalNumberOfMissedPoints();
  size_t pointCount                   = reconstruct.getPointCount();
  if ( ( losslessAtt || lossyMissedPointsPatch ) && useMissedPointsSeparateVideo ) {
    pointCount = frame.getTotalNumberOfRegularPoints();
  }
  if ( !pointCount || !reconstruct.hasColors() ) { return false; }

  const size_t curNumOfVideoFrames = video.getFrameCount();
  if ( params_.multipleStreams_ ) {
    video.resize( curNumOfVideoFrames + 1 );  // adding 1 more
    auto& image = video.getFrame( curNumOfVideoFrames );
    image.resize( frame.getWidth(), frame.getHeight() );
    image.set( 0 );
    videoT1.resize( curNumOfVideoFrames + 1 );  // adding 1 more
    auto& image1 = videoT1.getFrame( curNumOfVideoFrames );
    image1.resize( frame.getWidth(), frame.getHeight() );
    image1.set( 0 );

  } else {
    video.resize( curNumOfVideoFrames + mapCount );  // adding mapCount more
    for ( size_t f = 0; f < mapCount; ++f ) {
      auto& image = video.getFrame( f + curNumOfVideoFrames );
      image.resize( frame.getWidth(), frame.getHeight() );
      image.set( 0 );
    }
  }

  std::vector<bool> markT1;
  if ( params_.mapCountMinus1_ > 0 && params_.removeDuplicatePoints_ ) {
    const size_t size = frame.getWidth() * frame.getHeight();
    markT1.resize( size );
    for ( size_t i = 0; i < size; i++ ) { markT1[i] = false; }
  }

  for ( size_t i = 0; i < pointCount; ++i ) {
    const PCCVector3<size_t> location = pointToPixel[i];
    const PCCColor3B         color    = reconstruct.getColor( i );
    const size_t             u        = location[0];
    const size_t             v        = location[1];
    const size_t             f        = location[2];
    if ( params_.singleMapPixelInterleaving_ ) {
      if ( ( f == 0 && ( ( u + v ) % 2 == 0 ) ) || ( f == 1 && ( ( u + v ) % 2 == 1 ) ) ) {
        auto& image = video.getFrame( curNumOfVideoFrames );
        image.setValue( 0, u, v, color[0] );
        image.setValue( 1, u, v, color[1] );
        image.setValue( 2, u, v, color[2] );
      }
    } else {
      if ( f < mapCount ) {
        auto& image = params_.multipleStreams_ ? ( ( f == 0 ) ? video.getFrame( curNumOfVideoFrames )
                                                              : videoT1.getFrame( curNumOfVideoFrames ) )
                                               : video.getFrame( f + curNumOfVideoFrames );
        image.setValue( 0, u, v, color[0] );
        image.setValue( 1, u, v, color[1] );
        image.setValue( 2, u, v, color[2] );
        if ( params_.mapCountMinus1_ > 0 && params_.removeDuplicatePoints_ ) {
          auto& image1 = params_.multipleStreams_ ? videoT1.getFrame( curNumOfVideoFrames )
                                                  : video.getFrame( 1 + curNumOfVideoFrames );
          if ( f == 0 ) {
            if ( !markT1[v * frame.getWidth() + u] ) {
              image1.setValue( 0, u, v, color[0] );
              image1.setValue( 1, u, v, color[1] );
              image1.setValue( 2, u, v, color[2] );
            }
          } else {
            markT1[v * frame.getWidth() + u] = true;
          }
        }
      }
    }
  }
  if ( ( losslessAtt || lossyMissedPointsPatch ) && useMissedPointsSeparateVideo ) {
    // missed points
    std::cout.flush();
    std::vector<PCCColor3B>& mpsTextures = frame.getMpsTextures();
    std::vector<PCCColor3B>& eddTextures = frame.getEddTextures();
    std::cout.flush();
    mpsTextures.resize( numOfMPGeos );
    eddTextures.resize( numberOfEddPoints );

    for ( size_t i = 0; i < numOfMPGeos; ++i ) {
      const PCCColor3B color = reconstruct.getColor( pointCount + numberOfEddPoints + i );
      mpsTextures[i]         = color;
    }
    if ( params_.enhancedDeltaDepthCode_ ) {
      for ( size_t i = 0; i < numberOfEddPoints; ++i ) {
        const PCCColor3B color = reconstruct.getColor( pointCount + i );
        eddTextures[i]         = color;
      }
    }
  }
  return true;
}

void PCCEncoder::generateIntraEnhancedDeltaDepthImage( PCCFrameContext&        frame,
                                                       const PCCImageGeometry& imageRef,
                                                       PCCImageGeometry&       image ) {
  size_t width  = frame.getWidth();
  size_t height = frame.getHeight();
  image.resize( width, height );
  image.set( 0 );
  //  const int16_t infiniteDepth = ( std::numeric_limits<int16_t>::max )();
  for ( auto& patch : frame.getPatches() ) {
    for ( size_t v = 0; v < patch.getSizeV(); ++v ) {
      for ( size_t u = 0; u < patch.getSizeU(); ++u ) {
        const size_t  p = v * patch.getSizeU() + u;
        const int16_t d = patch.getDepth( 0 )[p];
        if ( d < infiniteDepth ) {
          const int16_t enhancedDeltaD = patch.getDepthEnhancedDeltaD()[p];
          size_t        x, y;
          patch.patch2Canvas( u, v, width, height, x, y );
          image.setValue( 0, x, y, uint16_t( enhancedDeltaD + imageRef.getValue( 0, x, y ) ) );
        }
      }
    }
  }

  if ( !frame.getUseMissedPointsSeparateVideo() ) {
    size_t numberOfMpsPatches = frame.getNumberOfMissedPointsPatches();
    for ( int i = 0; i < numberOfMpsPatches; i++ ) {
      auto&        missedPointsPatch = frame.getMissedPointsPatch( i );
      const size_t v0                = missedPointsPatch.v0_ * missedPointsPatch.occupancyResolution_;
      const size_t u0                = missedPointsPatch.u0_ * missedPointsPatch.occupancyResolution_;
      if ( missedPointsPatch.size() ) {
        for ( size_t v = 0; v < missedPointsPatch.sizeV_; ++v ) {
          for ( size_t u = 0; u < missedPointsPatch.sizeU_; ++u ) {
            const size_t p = v * missedPointsPatch.sizeU_ + u;
            if ( missedPointsPatch.x_[p] < infiniteDepth ) {
              const size_t x = ( u0 + u );
              const size_t y = ( v0 + v );
              assert( x < width && y < height );
              image.setValue( 0, x, y, uint16_t( missedPointsPatch.x_[p] ) );
              if ( params_.losslessGeo444_ ) {
                image.setValue( 1, x, y, uint16_t( missedPointsPatch.y_[p] ) );
                image.setValue( 2, x, y, uint16_t( missedPointsPatch.z_[p] ) );
              }
            }
          }
        }
      }
    }
  }
  return;
}

void PCCEncoder::performDataAdaptiveGPAMethod( PCCContext& context ) {
  // some valid parameters;
  SubContext    subContextPre, subContextCur;  // [start, end);
  unionPatch    unionPatchPre, unionPatchCur;  // [trackIndex, patchUnion];
  GlobalPatches globalPatchTracks;             // [trackIndex, <[frameIndex, patchIndex]>];
  bool          startSubContext      = true;   // startSubContext is initialized as true;  start a subContext;
  bool          endSubContext        = false;  // endSubContext   is initialized as false; end   a subContext;
  int           preSubcontextFrameId = -1;

  // iterate over all frameContexts;
  for ( size_t frameIndex = 0; frameIndex < context.size(); ++frameIndex ) {
    bool useRefFrame = true /*params_.keepGPARotation_*/;
    // determine whether start a subContext or not;
    if ( startSubContext ) {
      initializeSubContext( context[frameIndex], subContextPre, globalPatchTracks, unionPatchPre, frameIndex );

      if ( subContextPre.first == 0 ) { useRefFrame = false; }
      packingFirstFrame( context, frameIndex, params_.packingStrategy_, params_.safeGuardDistance_, useRefFrame );

      context[subContextPre.first].getPrePCCGPAFrameSize() = context[subContextPre.first].getCurPCCGPAFrameSize();
      context[subContextPre.first].getCurPCCGPAFrameSize().widthGPA_  = 0;
      context[subContextPre.first].getCurPCCGPAFrameSize().heightGPA_ = 0;
      for ( auto& patch : context[subContextPre.first].getPatches() ) {
        patch.getPreGPAPatchData() = patch.getCurGPAPatchData();
        patch.getCurGPAPatchData().initialize();
      }
      if ( frameIndex == context.size() - 1 ) {
        context.getSubContexts().emplace_back( subContextPre );
        updatePatchInformation( context, subContextPre );
        break;
      }
      subContextCur   = subContextPre;
      startSubContext = false;
      continue;
    }

    subContextCur.first  = subContextPre.first;
    subContextCur.second = frameIndex + 1;
    preSubcontextFrameId = subContextCur.first - 1;
    if ( subContextCur.first == 0 ) {
      useRefFrame          = false;
      preSubcontextFrameId = -1;
    }

    // clear current information;
    clearCurrentGPAPatchDataInfor( context, subContextCur );

    // genrate globalPatchTracks;
    size_t preIndex =
        frameIndex - subContextCur.first - 1;  // preIndex is the previous index in the current subcontext.
    generateGlobalPatches( context, frameIndex, globalPatchTracks, preIndex );

    // patch unions generation and packing;
    size_t unionsHeight = unionPatchGenerationAndPacking(
        globalPatchTracks, context, unionPatchCur, preSubcontextFrameId, params_.safeGuardDistance_, useRefFrame );

    // perform GPA packing;
    bool badPatchCount   = false;
    bool badUnionsHeight = false;
    bool badGPAPacking   = false;
    if ( double( unionPatchCur.size() ) / globalPatchTracks.size() < 0.15 ) { badPatchCount = true; }
    if ( unionsHeight > params_.minimumImageHeight_ ) { badUnionsHeight = true; }
    if ( printDetailedInfo ) {
      std::cout << "badPatchCount: " << badPatchCount << "badUnionsHeight: " << badUnionsHeight << std::endl;
    }
    if ( !badPatchCount && !badUnionsHeight ) {
      // patch information updating;
      updateGPAPatchInformation( context, subContextCur, unionPatchCur );

      // save the data into preGPAPatchData.
      performGPAPacking( subContextCur, unionPatchCur, context, badGPAPacking, unionsHeight, params_.safeGuardDistance_,
                         useRefFrame );
    }

    endSubContext = ( badPatchCount || badUnionsHeight || badGPAPacking );
    std::cout << "The endSubContext is: " << endSubContext << std::endl;

    if ( endSubContext ) {
      std::cout << "The frame is a end point --- " << frameIndex << std::endl;
      // clear current information;
      clearCurrentGPAPatchDataInfor( context, subContextCur );
      assert( subContextCur.second - subContextCur.first > 1 );

      subContextCur.first  = 0;
      subContextCur.second = 0;
      unionPatchCur.clear();
      globalPatchTracks.clear();  // GlobalPatches.......;
      // retain previous information;
      context.getSubContexts().emplace_back( subContextPre );  // SubContext..........;
      startSubContext = true;
      endSubContext   = false;
      frameIndex -= 1;  // should stay at the start point for next subcontext.

      // update Patch information;
      updatePatchInformation( context, subContextPre );
    } else {
      std::cout << "The frame " << frameIndex << " is not a end point ---" << std::endl;
      // previous information updating;
      for ( size_t j = subContextCur.first; j < subContextCur.second; ++j ) {
        auto& curPatches                   = context[j].getPatches();
        context[j].getPrePCCGPAFrameSize() = context[j].getCurPCCGPAFrameSize();
        assert( !curPatches.empty() );
        for ( auto& curPatch : curPatches ) { curPatch.getPreGPAPatchData() = curPatch.getCurGPAPatchData(); }
        if ( context[j].getMissedPointsPatches().size() > 0 && !context[j].getUseMissedPointsSeparateVideo() ) {
          for ( size_t idxRawPatches = 0; idxRawPatches < context[j].getMissedPointsPatches().size();
                idxRawPatches++ ) {
            context[j].getMissedPointsPatch( idxRawPatches ).preV0_ =
                context[j].getMissedPointsPatch( idxRawPatches ).tempV0_;
          }
        }
      }
      subContextPre = subContextCur;
      unionPatchPre.clear();
      unionPatchPre = unionPatchCur;
      std::cout << "cleared current tried infor:" << std::endl;
      // clear current information;
      for ( size_t j = subContextCur.first; j < subContextCur.second; ++j ) {
        auto& curPatches = context[j].getPatches();
        assert( !curPatches.empty() );
        for ( auto& curPatch : curPatches ) { curPatch.getCurGPAPatchData().initialize(); }
      }
      subContextCur.first  = 0;
      subContextCur.second = 0;
      unionPatchCur.clear();
      // the ending......;
      if ( frameIndex == ( context.size() - 1 ) ) {
        context.getSubContexts().emplace_back( subContextPre );  // SubContext..........;
        std::cout << "This is the last frame......." << std::endl;

        // update information;
        updatePatchInformation( context, subContextPre );
        break;
      }
    }
  }
}

void PCCEncoder::initializeSubContext( PCCFrameContext& frameContext,
                                       SubContext&      subContext,
                                       GlobalPatches&   globalPatchTracks,
                                       unionPatch&      unionPatch,
                                       size_t           frameIndex ) {
  // 1. initialize subContext;
  subContext.first  = frameIndex;
  subContext.second = frameIndex + 1;
  std::cout << "New subContext:[" << subContext.first << "," << subContext.second << ")" << std::endl;

  // 2. initialize globalPatchTracks && unionPatch;
  unionPatch.clear();
  globalPatchTracks.clear();
  for ( size_t patchIndex = 0; patchIndex < frameContext.getPatches().size(); ++patchIndex ) {
    globalPatchTracks[patchIndex].emplace_back( std::make_pair( frameIndex, patchIndex ) );
    frameContext.getPatches()[patchIndex].getCurGPAPatchData().isGlobalPatch    = true;
    frameContext.getPatches()[patchIndex].getCurGPAPatchData().globalPatchIndex = patchIndex;
  }
}
void PCCEncoder::clearCurrentGPAPatchDataInfor( PCCContext& context, SubContext& subContext ) {
  // clear current information;
  for ( size_t j = subContext.first; j < subContext.second; ++j ) {
    auto& curPatches = context[j].getPatches();
    assert( !curPatches.empty() );
    for ( auto& curPatch : curPatches ) { curPatch.getCurGPAPatchData().initialize(); }
    if ( !context[j].getMissedPointsPatches().empty() )
      for ( auto missedPointsPatch : context[j].getMissedPointsPatches() ) missedPointsPatch.tempV0_ = 0;
  }
}
void PCCEncoder::generateGlobalPatches( PCCContext&    context,
                                        size_t         frameIndex,
                                        GlobalPatches& globalPatchTracks,
                                        size_t         preIndex ) {
  auto& curPatches = context[frameIndex].getPatches();
  assert( curPatches.size() > 0 );
  for ( GlobalPatches::iterator iter = globalPatchTracks.begin(); iter != globalPatchTracks.end(); iter++ ) {
    auto& trackPatches = iter->second;  // !!!< <frameIndex, patchIndex> >;
    if ( trackPatches.empty() ) { continue; }
    const auto& preGlobalPatch = trackPatches[preIndex];
    const auto& prePatch       = context[preGlobalPatch.first].getPatches()[preGlobalPatch.second];
    float       thresholdIOU   = 0.2;
    float       maxIou         = 0.0;
    int         bestIdx        = -1;       // best matched patch index in curPatches;
    int         cId            = 0;        // patch index in curPatches;
    for ( auto& curPatch : curPatches ) {  // curPatches; may be modified;
      if ( prePatch.getViewId() == curPatch.getViewId() && !( curPatch.getCurGPAPatchData().isMatched ) &&
           ( prePatch.getLodScaleX() == curPatch.getLodScaleX() &&
             prePatch.getLodScaleY() == curPatch.getLodScaleY() ) ) {
        Rect  preRect = Rect( prePatch.getU1(), prePatch.getV1(), prePatch.getSizeU(), prePatch.getSizeV() );
        Rect  curRect = Rect( curPatch.getU1(), curPatch.getV1(), curPatch.getSizeU(), curPatch.getSizeV() );
        float iou     = computeIOU( preRect, curRect );
        if ( iou > maxIou ) {
          maxIou  = iou;
          bestIdx = cId;
        }
      }
      cId++;
    }
    if ( maxIou > thresholdIOU ) {                                // !!!best match found;
      curPatches[bestIdx].getCurGPAPatchData().isMatched = true;  // indicating the patch is already matched;
      trackPatches.emplace_back( std::make_pair( frameIndex, bestIdx ) );
    } else {
      trackPatches.clear();
    }
  }

  // update global patch information according to curGlobalPatches;
  for ( GlobalPatches::iterator iter = globalPatchTracks.begin(); iter != globalPatchTracks.end(); iter++ ) {
    const size_t trackIndex   = iter->first;
    const auto&  trackPatches = iter->second;  // !!!< <frameIndex, patchIndex> >;
    if ( trackPatches.empty() ) { continue; }
    for ( const auto& trackPatch : trackPatches ) {
      GPAPatchData& curGPAPatchData    = context[trackPatch.first].getPatches()[trackPatch.second].getCurGPAPatchData();
      curGPAPatchData.isGlobalPatch    = true;
      curGPAPatchData.globalPatchIndex = trackIndex;
    }
  }
}

size_t PCCEncoder::unionPatchGenerationAndPacking( const GlobalPatches& globalPatchTracks,
                                                   PCCContext&          context,
                                                   unionPatch&          unionPatchTemp,
                                                   size_t               refFrameIdx,
                                                   int                  safeguard,
                                                   bool                 useRefFrame ) {
  // 1. unionPatch generation;
  unionPatchTemp.clear();
  // 1.1 patchTracks generation;
  std::map<size_t, std::vector<PCCPatch>> patchTracks;
  for ( GlobalPatches::const_iterator iter = globalPatchTracks.begin(); iter != globalPatchTracks.end(); iter++ ) {
    const auto& trackIndex   = iter->first;
    const auto& trackPatches = iter->second;
    if ( trackPatches.empty() ) { continue; }
    for ( const auto& trackPatch : trackPatches ) {
      patchTracks[trackIndex].emplace_back( context[trackPatch.first].getPatches()[trackPatch.second] );
    }
  }
  // 1.2 union processing --- patchTracks -> unionPatch;
  for ( std::map<size_t, std::vector<PCCPatch>>::const_iterator iter = patchTracks.begin(); iter != patchTracks.end();
        iter++ ) {
    const auto& trackIndex   = iter->first;
    const auto& trackPatches = iter->second;
    assert( trackPatches.size() != 0 );
    // get the sizeU0 && sizeV0;
    size_t maxSizeU0 = 0;
    size_t maxSizeV0 = 0;
    for ( const auto& trackPatch : trackPatches ) {
      maxSizeU0 = std::max<size_t>( maxSizeU0, trackPatch.getSizeU0() );
      maxSizeV0 = std::max<size_t>( maxSizeV0, trackPatch.getSizeV0() );
    }

    // get the patch union;
    PCCPatch curPatchUnion;
    curPatchUnion.getIndex()  = trackIndex;
    curPatchUnion.getSizeU0() = maxSizeU0;
    curPatchUnion.getSizeV0() = maxSizeV0;
    curPatchUnion.getOccupancy().resize( maxSizeU0 * maxSizeV0, false );
    if ( useRefFrame && ( trackPatches.size() > 0 ) ) {
      assert( refFrameIdx != -1 );
      size_t matchedPatchIdx = trackPatches[0].getBestMatchIdx();  // the first frame in the subcontext.
      if ( matchedPatchIdx == -1 ) {
        curPatchUnion.getPatchOrientation() = -1;
      } else {  // suppose the refFrame is the same frame for all patches.
        curPatchUnion.getPatchOrientation() = context[refFrameIdx].getPatches()[matchedPatchIdx].getPatchOrientation();
        if ( printDetailedInfo ) {
          std::cout << "Maintained orientation for curPatchUnion.getPatchOrientation() = "
                    << curPatchUnion.getPatchOrientation() << std::endl;
        }
      }
    }
    for ( const auto& trackPatch : trackPatches ) {
      const auto& occupancy = trackPatch.getOccupancy();
      for ( size_t v = 0; v < trackPatch.getSizeV0(); ++v ) {
        for ( size_t u = 0; u < trackPatch.getSizeU0(); ++u ) {
          assert( v < maxSizeV0 );
          assert( u < maxSizeU0 );
          size_t p  = v * trackPatch.getSizeU0() + u;
          size_t up = v * curPatchUnion.getSizeU0() + u;
          if ( occupancy[p] && !( curPatchUnion.getOccupancy()[up] ) ) curPatchUnion.getOccupancy()[up] = true;
        }
      }
    }
    unionPatchTemp[trackIndex] = curPatchUnion;
  }

  // 2. unionPatch packing;
  size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t occupancySizeV = 0;  // GPA_HARMONIZATION
  for ( unionPatch::const_iterator iter = unionPatchTemp.begin(); iter != unionPatchTemp.end(); iter++ ) {
    const auto& curPatchUnion = iter->second;
    occupancySizeU            = std::max<size_t>( occupancySizeU, curPatchUnion.getSizeU0() + 1 );
    occupancySizeV            = std::max<size_t>( occupancySizeV, curPatchUnion.getSizeV0() + 1 );
  }
  size_t width  = occupancySizeU * params_.occupancyResolution_;
  size_t height = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  std::vector<bool> occupancyMap;
  vector<int>       orientation_vertical = {
      PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
  vector<int> orientation_horizontal = {
      PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};  // favoring horizontal orientations (that should be
                                                             // rotated)
  int numOrientations = params_.useEightOrientations_ ? 8 : 2;
  occupancyMap.resize( occupancySizeU * occupancySizeV, false );
  for ( unionPatch::iterator iter = unionPatchTemp.begin(); iter != unionPatchTemp.end(); iter++ ) {
    auto& curPatchUnion = iter->second;  // [u0, v0] may be modified;
    assert( curPatchUnion.getSizeU0() < occupancySizeU );
    assert( curPatchUnion.getSizeV0() < occupancySizeV );
    bool  locationFound = false;
    auto& occupancy     = curPatchUnion.getOccupancy();
    while ( !locationFound ) {
      for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
        for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
          curPatchUnion.getU0() = u;
          curPatchUnion.getV0() = v;
          if ( params_.packingStrategy_ == 0 ) {
            curPatchUnion.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;
            if ( curPatchUnion.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                    params_.lowDelayEncoding_, safeguard ) ) {
              locationFound = true;
              if ( printDetailedInfo ) {
                std::cout << "Orientation " << curPatchUnion.getPatchOrientation() << " selected for unionPatch "
                          << curPatchUnion.getIndex() << " (" << u << "," << v << ")" << std::endl;
              }
            }
          } else {
            if ( useRefFrame && ( curPatchUnion.getPatchOrientation() != -1 ) ) {
              // already knonw Patch Orientation. just try.
              if ( curPatchUnion.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                      params_.lowDelayEncoding_, safeguard ) ) {
                locationFound = true;
                if ( printDetailedInfo ) {
                  std::cout << "location u0,v0 selected for unionPatch " << curPatchUnion.getIndex() << " (" << u << ","
                            << v << ")" << std::endl;
                }
              }
            } else {
              for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound; orientationIdx++ ) {
                if ( curPatchUnion.getSizeU0() > curPatchUnion.getSizeV0() ) {
                  curPatchUnion.getPatchOrientation() = orientation_horizontal[orientationIdx];
                } else {
                  curPatchUnion.getPatchOrientation() = orientation_vertical[orientationIdx];
                }
                if ( curPatchUnion.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV,
                                                        params_.lowDelayEncoding_, safeguard ) ) {
                  locationFound = true;
                  if ( printDetailedInfo ) {
                    std::cout << "Orientation " << curPatchUnion.getPatchOrientation() << " selected for unionPatch "
                              << curPatchUnion.getIndex() << " (" << u << "," << v << ")" << std::endl;
                  }
                }
              }
            }
          }
        }
      }
      if ( !locationFound ) {
        occupancySizeV *= 2;
        occupancyMap.resize( occupancySizeU * occupancySizeV );
      }
    }
    for ( size_t v0 = 0; v0 < curPatchUnion.getSizeV0(); ++v0 ) {
      for ( size_t u0 = 0; u0 < curPatchUnion.getSizeU0(); ++u0 ) {
        int coord = curPatchUnion.patchBlock2CanvasBlock( u0, v0, occupancySizeU, occupancySizeV );
        if ( params_.lowDelayEncoding_ )
          occupancyMap[coord] = true;
        else
          occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * curPatchUnion.getSizeU0() + u0];
      }
    }

    if ( !( curPatchUnion.isPatchDimensionSwitched() ) ) {
      height =
          ( std::max )( height, ( curPatchUnion.getV0() + curPatchUnion.getSizeV0() ) * params_.occupancyResolution_ );
      width =
          ( std::max )( width, ( curPatchUnion.getU0() + curPatchUnion.getSizeU0() ) * params_.occupancyResolution_ );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curPatchUnion.getV0() + curPatchUnion.getSizeV0() ) );
    } else {
      height =
          ( std::max )( height, ( curPatchUnion.getV0() + curPatchUnion.getSizeU0() ) * params_.occupancyResolution_ );
      width =
          ( std::max )( width, ( curPatchUnion.getU0() + curPatchUnion.getSizeV0() ) * params_.occupancyResolution_ );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curPatchUnion.getV0() + curPatchUnion.getSizeU0() ) );
    }
  }
  std::cout << "actualImageSize " << width << " x " << height << std::endl;
  return height;
}

void PCCEncoder::packingFirstFrame( PCCContext& context,
                                    size_t      frameIndex,
                                    bool        packingStrategy,
                                    int         safeguard,
                                    bool        hasRefFrame ) {
  PCCFrameContext& frame          = context[frameIndex];
  auto&            patches        = frame.getPatches();
  size_t           occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
  size_t           occupancySizeV = ( std::max )( patches[0].getSizeU0(), patches[0].getSizeV0() );

  for ( auto& patch : patches ) { occupancySizeU = ( std::max )( occupancySizeU, patch.getSizeU0() + 1 ); }
  auto& widthGPA = frame.getCurPCCGPAFrameSize().widthGPA_;
  auto& heithGPA = frame.getCurPCCGPAFrameSize().heightGPA_;
  widthGPA       = occupancySizeU * params_.occupancyResolution_;
  heithGPA       = occupancySizeV * params_.occupancyResolution_;
  size_t maxOccupancyRow{0};

  if ( packingStrategy == 0 ) {
    std::vector<bool> occupancyMap;
    occupancyMap.resize( occupancySizeU * occupancySizeV, false );
    for ( auto& patch : patches ) {
      assert( patch.getSizeU0() <= occupancySizeU );
      assert( patch.getSizeV0() <= occupancySizeV );
      bool  locationFound = false;
      auto& occupancy     = patch.getOccupancy();
      while ( !locationFound ) {
        patch.getPatchOrientation() = PATCH_ORIENTATION_DEFAULT;  // only one orientation is allowed
        for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
          for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
            patch.getU0() = u;
            patch.getV0() = v;
            if ( patch.checkFitPatchCanvas( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                            safeguard ) ) {
              locationFound = true;
            }
          }
        }
        if ( !locationFound ) {
          occupancySizeV *= 2;
          occupancyMap.resize( occupancySizeU * occupancySizeV );
        }
      }
      for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
        const size_t v = patch.getV0() + v0;
        for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
          const size_t u = patch.getU0() + u0;
          if ( params_.lowDelayEncoding_ )
            occupancyMap[v * occupancySizeU + u] = true;
          else
            occupancyMap[v * occupancySizeU + u] =
                occupancyMap[v * occupancySizeU + u] || occupancy[v0 * patch.getSizeU0() + u0];
        }
      }

      heithGPA = ( std::max )( heithGPA, ( patch.getV0() + patch.getSizeV0() ) * patch.getOccupancyResolution() );
      widthGPA = ( std::max )( widthGPA, ( patch.getU0() + patch.getSizeU0() ) * patch.getOccupancyResolution() );
      maxOccupancyRow = ( std::max )( maxOccupancyRow, ( patch.getV0() + patch.getSizeV0() ) );
    }

    if ( frame.getMissedPointsPatches().size() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
      packMissedPointsPatch( frame, occupancyMap, widthGPA, heithGPA, occupancySizeU, occupancySizeV, maxOccupancyRow );
      for ( size_t idxRawPatches = 0; idxRawPatches < frame.getMissedPointsPatches().size(); idxRawPatches++ ) {
        frame.getMissedPointsPatch( idxRawPatches ).preV0_ = frame.getMissedPointsPatch( idxRawPatches ).v0_;
      }
    } else {
      if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
    }
    if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
      packEOMTexturePointsPatch( frame, occupancyMap, widthGPA, heithGPA, occupancySizeU, occupancySizeV,
                                 maxOccupancyRow );
    }
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
    std::cout << "actualImageSize " << widthGPA << " x " << heithGPA << std::endl;
  } else {
    vector<int> orientation_vertical = {
        PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
        PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
        PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
    vector<int> orientation_horizontal = {
        PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
        PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
        PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};  // favoring horizontal orientations (that should be
                                                               // rotated)
    int numOrientations = params_.useEightOrientations_ ? 8 : 2;

    std::vector<bool> occupancyMap;
    occupancyMap.resize( occupancySizeU * occupancySizeV, false );

    for ( auto& patch : patches ) {
      assert( patch.getSizeU0() <= occupancySizeU );
      assert( patch.getSizeV0() <= occupancySizeV );
      bool          locationFound   = false;
      auto&         occupancy       = patch.getOccupancy();
      GPAPatchData& curGPAPatchData = patch.getCurGPAPatchData();
      curGPAPatchData.occupancy     = occupancy;
      curGPAPatchData.sizeU0        = patch.getSizeU0();
      curGPAPatchData.sizeV0        = patch.getSizeV0();

      while ( !locationFound ) {
        // no ref patch for first frame.
        if ( ( patch.getBestMatchIdx() != InvalidPatchIndex ) && ( hasRefFrame ) ) {
          std::vector<PCCPatch>& prevPatches = context[frameIndex - 1].getPatches();
          curGPAPatchData.patchOrientation   = prevPatches[patch.getBestMatchIdx()].getPatchOrientation();
          // try to place on the same position as the matched patch
          curGPAPatchData.u0 = prevPatches[patch.getBestMatchIdx()].getU0();
          curGPAPatchData.v0 = prevPatches[patch.getBestMatchIdx()].getV0();
          if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV,
                                                params_.lowDelayEncoding_ ) ) {
            locationFound = true;
            if ( printDetailedInfo ) {
              std::cout << "Maintained orientation " << curGPAPatchData.patchOrientation
                        << " for matched patch in the same position (" << curGPAPatchData.u0 << ","
                        << curGPAPatchData.v0 << ")" << std::endl;
            }
          }
          // if the patch couldn't fit, try to fit the patch in the top left position
          for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
            for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
              curGPAPatchData.u0 = u;
              curGPAPatchData.v0 = v;
              if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV,
                                                    params_.lowDelayEncoding_, safeguard ) ) {
                locationFound = true;
                if ( printDetailedInfo ) {
                  std::cout << "Maintained orientation " << curGPAPatchData.patchOrientation << " for matched patch:("
                            << curGPAPatchData.u0 << "," << curGPAPatchData.v0 << ")" << std::endl;
                }
              }
            }
          }
        } else {
          // best effort
          for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
            for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
              curGPAPatchData.u0 = u;
              curGPAPatchData.v0 = v;
              for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound; orientationIdx++ ) {
                if ( curGPAPatchData.sizeU0 > curGPAPatchData.sizeV0 ) {
                  curGPAPatchData.patchOrientation = orientation_horizontal[orientationIdx];
                } else {
                  curGPAPatchData.patchOrientation = orientation_vertical[orientationIdx];
                }
                if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV,
                                                      params_.lowDelayEncoding_, safeguard ) ) {
                  locationFound = true;
                  if ( printDetailedInfo ) {
                    std::cout << "Orientation " << curGPAPatchData.patchOrientation << " selected for unmatched patch:("
                              << curGPAPatchData.u0 << "," << curGPAPatchData.v0 << ")" << std::endl;
                  }
                }
              }
            }
          }
        }
        if ( !locationFound ) {
          occupancySizeV *= 2;
          occupancyMap.resize( occupancySizeU * occupancySizeV );
        }
      }
      for ( size_t v0 = 0; v0 < curGPAPatchData.sizeV0; ++v0 ) {
        for ( size_t u0 = 0; u0 < curGPAPatchData.sizeU0; ++u0 ) {
          int coord = patch.patchBlock2CanvasBlockForGPA( u0, v0, occupancySizeU, occupancySizeV );
          if ( params_.lowDelayEncoding_ )
            occupancyMap[coord] = true;
          else
            occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
        }
      }
      if ( !( curGPAPatchData.isPatchDimensionSwitched() ) ) {
        heithGPA =
            ( std::max )( heithGPA, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
        widthGPA =
            ( std::max )( widthGPA, ( curGPAPatchData.u0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) );
      } else {
        heithGPA =
            ( std::max )( heithGPA, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
        widthGPA =
            ( std::max )( widthGPA, ( curGPAPatchData.u0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
        maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) );
      }
    }

    if ( frame.getMissedPointsPatches().size() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
      packMissedPointsPatch( frame, occupancyMap, widthGPA, heithGPA, occupancySizeU, occupancySizeV, maxOccupancyRow );
      for ( size_t idxRawPatches = 0; idxRawPatches < frame.getMissedPointsPatches().size(); idxRawPatches++ ) {
        frame.getMissedPointsPatch( idxRawPatches ).preV0_ = frame.getMissedPointsPatch( idxRawPatches ).v0_;
      }
    } else {
      if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
    }
    if ( params_.enhancedDeltaDepthCode_ && !frame.getUseMissedPointsSeparateVideo() ) {
      packEOMTexturePointsPatch( frame, occupancyMap, widthGPA, heithGPA, occupancySizeU, occupancySizeV,
                                 maxOccupancyRow );
    }
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }

    std::cout << "actualImageSize " << widthGPA << " x " << heithGPA << std::endl;
  }
}

void PCCEncoder::updatePatchInformation( PCCContext& context, SubContext& subContext ) {
  std::cout << "The subContext is: [" << subContext.first << ", " << subContext.second << ")" << std::endl;
  for ( size_t frameIndex = subContext.first; frameIndex < subContext.second; ++frameIndex ) {
    PCCFrameContext& frame      = context[frameIndex];
    frame.getGlobalPatchCount() = 0;  // GPA_HARMONIZATION
    frame.getWidth()            = frame.getPrePCCGPAFrameSize().widthGPA_;
    frame.getHeight()           = frame.getPrePCCGPAFrameSize().heightGPA_;
    auto& curPatches            = frame.getPatches();
    for ( auto& curPatch : curPatches ) {
      GPAPatchData& preGPAPatchData  = curPatch.getPreGPAPatchData();
      curPatch.getSizeU0()           = preGPAPatchData.sizeU0;
      curPatch.getSizeV0()           = preGPAPatchData.sizeV0;
      curPatch.getOccupancy()        = preGPAPatchData.occupancy;
      curPatch.getU0()               = preGPAPatchData.u0;
      curPatch.getV0()               = preGPAPatchData.v0;
      curPatch.getPatchOrientation() = preGPAPatchData.patchOrientation;
      curPatch.getIsGlobalPatch()    = preGPAPatchData.isGlobalPatch;
      // GPA_HARMONIZATION Begin --------------------------------------
      if ( curPatch.getIsGlobalPatch() ) { frame.getGlobalPatchCount() = frame.getGlobalPatchCount() + 1; }
      // GPA_HARMONIZATION End ----------------------------------------
    }

    // update missedPoint patch infor.
    if ( frame.getMissedPointsPatches().size() > 0 && !frame.getUseMissedPointsSeparateVideo() ) {
      for ( size_t idxRawPatches = 0; idxRawPatches < frame.getMissedPointsPatches().size(); idxRawPatches++ ) {
        // frame.getMissedPointsPatch( idxRawPatches ).u0_ = frame.getMissedPointsPatch( idxRawPatches ).preU0_;
        frame.getMissedPointsPatch( idxRawPatches ).v0_ = frame.getMissedPointsPatch( idxRawPatches ).preV0_;
      }
    }
  }

  // GPA_HARMONIZATION Begin  --------------------------------------
  // no need to update single frame.
  if ( subContext.second - subContext.first == 1 ) {  // only one frame
    // Reset bestMatchIndex for the first frame.
    PCCFrameContext& frame      = context[subContext.first];
    auto&            curPatches = frame.getPatches();
    for ( auto& patch : curPatches ) { patch.setBestMatchIdx( InvalidPatchIndex ); }
    frame.setNumMatchedPatches( 0 );
    return;
  }
  int globalPatchCount = 0;
  for ( size_t frameIndex = subContext.first; frameIndex < subContext.second; frameIndex++ ) {
    PCCFrameContext& frame      = context[frameIndex];
    auto&            curPatches = frame.getPatches();
    for ( size_t index = 0; index < curPatches.size(); index++ ) { curPatches[index].getIndex() = index; }
    // reorder the patches.
    vector<PCCPatch> reorderPatches = curPatches;
    globalPatchCount                = frame.getGlobalPatchCount();
    std::cout << "frameIndex: " << frameIndex << " patchCount:" << reorderPatches.size();
    std::cout << " frame.getGlobalPatchCount() = " << globalPatchCount << std::endl;

    curPatches.clear();
    curPatches.resize( 0 );
    if ( frameIndex == subContext.first ) {
      for ( auto& patch : reorderPatches ) {
        if ( patch.getIsGlobalPatch() ) { curPatches.emplace_back( patch ); }
      }
      for ( auto& patch : reorderPatches ) {
        if ( !patch.getIsGlobalPatch() ) { curPatches.emplace_back( patch ); }
      }
    } else {
      // get global patch.
      for ( int32_t index = 0; index < context[frameIndex - 1].getPatches().size(); index++ ) {
        for ( auto& patch : reorderPatches ) {
          if ( ( index == patch.getBestMatchIdx() ) && ( patch.getIsGlobalPatch() ) ) {
            curPatches.emplace_back( patch );
            break;
          }
        }
      }
      // get non-global patch.
      for ( auto& patch : reorderPatches ) {
        if ( !patch.getIsGlobalPatch() ) { curPatches.emplace_back( patch ); }
      }
    }
  }

  // adjust index.
  for ( size_t frameIndex = subContext.first; frameIndex < subContext.second; frameIndex++ ) {
    auto& curPatches = context[frameIndex].getPatches();
    // global patch.
    for ( int32_t index = 0; index < globalPatchCount; index++ ) {
      if ( frameIndex > subContext.first ) { curPatches[index].setBestMatchIdx( index ); }
      curPatches[index].getIndex() = index;
    }
    if ( frameIndex == subContext.second - 1 ) {
      for ( int32_t index = globalPatchCount; index < curPatches.size(); index++ ) {
        curPatches[index].getIndex() = index;
      }
      continue;
    }

    // non-global patches.
    auto&             nextPatches = context[frameIndex + 1].getPatches();
    std::vector<bool> updated;
    updated.resize( nextPatches.size(), false );
    for ( int32_t index = globalPatchCount; index < curPatches.size(); index++ ) {
      for ( int32_t i = globalPatchCount; i < nextPatches.size(); i++ ) {
        if ( ( (int32_t)curPatches[index].getIndex() == nextPatches[i].getBestMatchIdx() ) &&
             ( updated[i] == false ) ) {
          nextPatches[i].setBestMatchIdx( index );
          updated[i] = true;
          break;
        }
      }
      curPatches[index].getIndex() = (size_t)index;
      std::cout << "[" << frameIndex << ":" << index << " ," << curPatches[index].getBestMatchIdx() << "] ";
    }
  }

  // Reset bestMatchIndex for the first frame.
  PCCFrameContext& frame      = context[subContext.first];
  auto&            curPatches = frame.getPatches();
  for ( auto& patch : curPatches ) { patch.setBestMatchIdx( InvalidPatchIndex ); }
  frame.setNumMatchedPatches( 0 );

  // Other frames in the current subContext
  for ( size_t frameIndex = subContext.first + 1; frameIndex < subContext.second; frameIndex++ ) {
    auto&  curPatches        = context[frameIndex].getPatches();
    size_t numMatchedPatches = globalPatchCount;
    for ( int32_t index = globalPatchCount; index < curPatches.size(); index++ ) {
      if ( curPatches[index].getBestMatchIdx() != InvalidPatchIndex ) numMatchedPatches++;
    }
    context[frameIndex].setNumMatchedPatches( numMatchedPatches );
  }

  // GPA_HARMONIZATION End ----------------------------------------

  return;
}

void PCCEncoder::updateGPAPatchInformation( PCCContext& context, SubContext& subContext, unionPatch& unionPatch ) {
  for ( size_t i = subContext.first; i < subContext.second; ++i ) {
    auto& patches = context[i].getPatches();
    for ( auto& patch : patches ) {
      GPAPatchData& curGPAPatchData = patch.getCurGPAPatchData();
      if ( curGPAPatchData.isGlobalPatch ) {
        size_t            globalIndex      = curGPAPatchData.globalPatchIndex;
        auto&             cPatchUnion      = unionPatch[globalIndex];
        size_t            initialSizeU0    = patch.getSizeU0();
        size_t            initialSizeV0    = patch.getSizeV0();
        size_t            updatedSizeU0    = cPatchUnion.getSizeU0();
        size_t            updatedSizeV0    = cPatchUnion.getSizeV0();
        auto&             initialOccupancy = patch.getOccupancy();
        std::vector<bool> updatedOccupancy( updatedSizeU0 * updatedSizeV0, false );
        for ( size_t v0 = 0; v0 < initialSizeV0; ++v0 ) {
          for ( size_t u0 = 0; u0 < initialSizeU0; ++u0 ) {
            size_t initialIndex = v0 * initialSizeU0 + u0;
            size_t updatedIndex = v0 * updatedSizeU0 + u0;
            if ( initialOccupancy[initialIndex] && !updatedOccupancy[updatedIndex] )
              updatedOccupancy[updatedIndex] = true;
          }
        }
        curGPAPatchData.sizeU0    = updatedSizeU0;
        curGPAPatchData.sizeV0    = updatedSizeV0;
        curGPAPatchData.occupancy = updatedOccupancy;
      } else {
        curGPAPatchData.sizeU0    = patch.getSizeU0();
        curGPAPatchData.sizeV0    = patch.getSizeV0();
        curGPAPatchData.occupancy = patch.getOccupancy();
      }
    }
  }
}

void PCCEncoder::performGPAPacking( const SubContext& subContext,
                                    unionPatch&       unionPatch,
                                    PCCContext&       context,
                                    bool&             badGPAPacking,
                                    size_t            unionsHeight,
                                    int               safeguard,
                                    bool              useRefFrame ) {
  bool   exceedMinimumImageHeight = false;  // whether exceed minimunImageHeight or not;
  size_t badCondition             = 0;      // GPA bad condition count;
  for ( size_t i = subContext.first; i < subContext.second; ++i ) {
    auto& curFrameContext = context[i];
    auto& widthGPA        = curFrameContext.getCurPCCGPAFrameSize().widthGPA_;
    auto& heightGPA       = curFrameContext.getCurPCCGPAFrameSize().heightGPA_;
    auto& patches         = curFrameContext.getPatches();
    if ( patches.empty() ) { return; }
    int   preIndex   = i > 0 ? i - 1 : 0;
    auto& prePatches = context[preIndex].getPatches();

    size_t occupancySizeU = params_.minimumImageWidth_ / params_.occupancyResolution_;
    size_t occupancySizeV = unionsHeight / params_.occupancyResolution_;
    for ( auto& patch : patches ) {
      occupancySizeU = ( std::max )( occupancySizeU, patch.getCurGPAPatchData().sizeU0 + 1 );
    }
    widthGPA  = occupancySizeU * params_.occupancyResolution_;
    heightGPA = occupancySizeV * params_.occupancyResolution_;
    size_t maxOccupancyRow{0};

    vector<int> orientation_vertical   = {PATCH_ORIENTATION_DEFAULT,
                                        PATCH_ORIENTATION_SWAP};  // favoring vertical orientation
    vector<int> orientation_horizontal = {
        PATCH_ORIENTATION_SWAP,
        PATCH_ORIENTATION_DEFAULT};  // favoring horizontal orientations (that should be rotated)
    std::vector<bool> occupancyMap;
    occupancyMap.resize( occupancySizeU * occupancySizeV, false );
    // !!!packing global matched patch;
    for ( auto& patch : patches ) {
      GPAPatchData& curGPAPatchData = patch.getCurGPAPatchData();
      if ( curGPAPatchData.isGlobalPatch ) {
        assert( curGPAPatchData.sizeU0 <= occupancySizeU );
        assert( curGPAPatchData.sizeV0 <= occupancySizeV );
        const size_t trackIndex = curGPAPatchData.globalPatchIndex;
        assert( unionPatch.count( trackIndex ) != 0 );
        curGPAPatchData.u0               = unionPatch[trackIndex].getU0();
        curGPAPatchData.v0               = unionPatch[trackIndex].getV0();
        curGPAPatchData.patchOrientation = unionPatch[trackIndex].getPatchOrientation();
        if ( printDetailedInfo ) {
          std::cout << "Orientation:" << curGPAPatchData.patchOrientation << " for GPA patch in the same position ("
                    << curGPAPatchData.u0 << "," << curGPAPatchData.v0 << ")" << std::endl;
        }
        for ( size_t v0 = 0; v0 < curGPAPatchData.sizeV0; ++v0 ) {
          for ( size_t u0 = 0; u0 < curGPAPatchData.sizeU0; ++u0 ) {
            int coord = patch.patchBlock2CanvasBlockForGPA( u0, v0, occupancySizeU, occupancySizeV );
            if ( params_.lowDelayEncoding_ )
              occupancyMap[coord] = true;
            else
              occupancyMap[coord] = occupancyMap[coord] || curGPAPatchData.occupancy[v0 * curGPAPatchData.sizeU0 + u0];
          }
        }
        if ( !( curGPAPatchData.isPatchDimensionSwitched() ) ) {
          heightGPA       = ( std::max )( heightGPA,
                                    ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
          widthGPA        = ( std::max )( widthGPA,
                                   ( curGPAPatchData.u0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) );
        } else {
          heightGPA       = ( std::max )( heightGPA,
                                    ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
          widthGPA        = ( std::max )( widthGPA,
                                   ( curGPAPatchData.u0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
          maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) );
        }
      }
    }
    // !!!packing non-global matched patch;
    int icount = -1;
    for ( auto& patch : patches ) {
      icount++;
      GPAPatchData& curGPAPatchData = patch.getCurGPAPatchData();
      if ( curGPAPatchData.isGlobalPatch ) { continue; }

      // not use reference frame only if the first frame or useRefFrame is disabled.
      if ( ( i == 0 ) || ( ( i == subContext.first ) && ( !useRefFrame ) ) ) {  // not use ref.
        packingWithoutRefForFirstFrameNoglobalPatch( patch, i, icount, occupancySizeU, occupancySizeV, safeguard,
                                                     occupancyMap, heightGPA, widthGPA, maxOccupancyRow );
      } else {
        // PCCPatch prePatch = prePatches[patch.getBestMatchIdx()];
        packingWithRefForFirstFrameNoglobalPatch( patch, prePatches, subContext.first, i, icount, occupancySizeU,
                                                  occupancySizeV, safeguard, occupancyMap, heightGPA, widthGPA,
                                                  maxOccupancyRow );
      }
    }

    if ( curFrameContext.getMissedPointsPatches().size() > 0 && !curFrameContext.getUseMissedPointsSeparateVideo() ) {
      packMissedPointsPatch( curFrameContext, occupancyMap, widthGPA, heightGPA, occupancySizeU, occupancySizeV,
                             maxOccupancyRow );
      for ( size_t idxRawPatches = 0; idxRawPatches < curFrameContext.getMissedPointsPatches().size();
            idxRawPatches++ ) {
        curFrameContext.getMissedPointsPatch( idxRawPatches ).tempV0_ =
            curFrameContext.getMissedPointsPatch( idxRawPatches ).v0_;
      }
    } else {
      if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }
    }
    if ( params_.enhancedDeltaDepthCode_ && !curFrameContext.getUseMissedPointsSeparateVideo() ) {
      packEOMTexturePointsPatch( curFrameContext, occupancyMap, widthGPA, heightGPA, occupancySizeU, occupancySizeV,
                                 maxOccupancyRow );
    }
    if ( printDetailedInfo ) { printMap( occupancyMap, occupancySizeU, occupancySizeV ); }

    // determination......;
    if ( heightGPA > params_.minimumImageHeight_ ) {
      exceedMinimumImageHeight = true;
      break;
    }
    double validHeightRatio = ( double( heightGPA ) ) / ( double( curFrameContext.getHeight() ) );
    if ( validHeightRatio >= BAD_HEIGHT_THRESHOLD ) { badCondition++; }
  }

  if ( exceedMinimumImageHeight || badCondition > BAD_CONDITION_THRESHOLD ) { badGPAPacking = true; }
}

void PCCEncoder::packingWithoutRefForFirstFrameNoglobalPatch(
    PCCPatch&          patch,
    size_t             i,
    size_t             icount,
    size_t&            occupancySizeU,
    size_t&            occupancySizeV,
    const size_t       safeguard,
    std::vector<bool>& occupancyMap,
    size_t&            heightGPA,
    size_t&            widthGPA,
    size_t&            maxOccupancyRow ) {  // GPA_HAMONIZATION, the whole function has been changed
  vector<int> orientation_vertical = {
      PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};  // favoring vertical orientation
  vector<int> orientation_horizontal = {
      PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
      PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
      PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};  // favoring horizontal orientations (that should be
                                                             // rotated)
  int           numOrientations = params_.useEightOrientations_ ? 8 : 2;
  GPAPatchData& curGPAPatchData =
      patch.getCurGPAPatchData();  // GPAPatchData& preGPAPatchData = patch.getCurGPAPatchData();

  assert( curGPAPatchData.sizeU0 <= occupancySizeU );
  assert( curGPAPatchData.sizeV0 <= occupancySizeV );
  bool  locationFound = false;
  auto& occupancy     = patch.getOccupancy();
  while ( !locationFound ) {
    for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
      for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
        curGPAPatchData.u0 = u;
        curGPAPatchData.v0 = v;
        if ( params_.packingStrategy_ == 0 ) {
          if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                                safeguard ) ) {
            locationFound = true;
            if ( printDetailedInfo ) {
              std::cout << "Orientation " << curGPAPatchData.patchOrientation << " selected for Patch: ["
                        << icount  // preGPAPatchData->curXXXX
                        << "] in the position (" << u << "," << v << ")" << std::endl;
            }
          }
        } else {  // try several orientation.
          for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound; orientationIdx++ ) {
            if ( patch.getSizeU0() > patch.getSizeV0() ) {
              curGPAPatchData.patchOrientation = orientation_horizontal[orientationIdx];
            } else {
              curGPAPatchData.patchOrientation = orientation_vertical[orientationIdx];
            }
            if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV,
                                                  params_.lowDelayEncoding_, safeguard ) ) {
              locationFound = true;
              if ( printDetailedInfo ) {
                std::cout << "Orientation " << curGPAPatchData.patchOrientation << "selected for Patch: [" << icount
                          << "] in the position (" << curGPAPatchData.u0 << "," << curGPAPatchData.v0 << ")"
                          << std::endl;
              }
            }
          }
        }
      }
    }
    if ( !locationFound ) {
      occupancySizeV *= 2;
      occupancyMap.resize( occupancySizeU * occupancySizeV );
      if ( printDetailedInfo ) { std::cout << "Increase occupancySizeV " << occupancySizeV << std::endl; }
    }
  }
  // update occupancy.
  for ( size_t v0 = 0; v0 < curGPAPatchData.sizeV0; ++v0 ) {
    for ( size_t u0 = 0; u0 < curGPAPatchData.sizeU0; ++u0 ) {
      int coord = patch.patchBlock2CanvasBlockForGPA( u0, v0, occupancySizeU, occupancySizeV );
      if ( params_.lowDelayEncoding_ )
        occupancyMap[coord] = true;
      else
        occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * patch.getSizeU0() + u0];
    }
  }
  if ( !( curGPAPatchData.isPatchDimensionSwitched() ) ) {
    heightGPA =
        ( std::max )( heightGPA, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
    widthGPA =
        ( std::max )( widthGPA, ( curGPAPatchData.u0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
    maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) );
  } else {
    heightGPA =
        ( std::max )( heightGPA, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
    widthGPA =
        ( std::max )( widthGPA, ( curGPAPatchData.u0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
    maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) );
  }
}
void PCCEncoder::packingWithRefForFirstFrameNoglobalPatch( PCCPatch&                   patch,
                                                           const std::vector<PCCPatch> prePatches,
                                                           size_t                      startFrameIndex,
                                                           size_t                      i,
                                                           size_t                      icount,
                                                           size_t&                     occupancySizeU,
                                                           size_t&                     occupancySizeV,
                                                           const size_t                safeguard,
                                                           std::vector<bool>&          occupancyMap,
                                                           size_t&                     heightGPA,
                                                           size_t&                     widthGPA,
                                                           size_t&                     maxOccupancyRow ) {
  vector<int> orientation_vertical = {PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_SWAP,    PATCH_ORIENTATION_ROT180,
                                      PATCH_ORIENTATION_MIRROR,  PATCH_ORIENTATION_MROT180, PATCH_ORIENTATION_ROT270,
                                      PATCH_ORIENTATION_MROT90,  PATCH_ORIENTATION_ROT90};
  // favoring vertical orientation
  vector<int> orientation_horizontal = {PATCH_ORIENTATION_SWAP,   PATCH_ORIENTATION_DEFAULT, PATCH_ORIENTATION_ROT270,
                                        PATCH_ORIENTATION_MROT90, PATCH_ORIENTATION_ROT90,   PATCH_ORIENTATION_ROT180,
                                        PATCH_ORIENTATION_MIRROR, PATCH_ORIENTATION_MROT180};
  // favoring horizontal orientations (that should be rotated)
  int32_t       numOrientations = params_.useEightOrientations_ ? 8 : 2;
  GPAPatchData& curGPAPatchData = patch.getCurGPAPatchData();  // GPA_HARMONIZATION

  assert( curGPAPatchData.sizeU0 <= occupancySizeU );
  assert( curGPAPatchData.sizeV0 <= occupancySizeV );
  bool  locationFound = false;
  auto& occupancy     = patch.getOccupancy();
  while ( !locationFound ) {
    if ( patch.getBestMatchIdx() != InvalidPatchIndex ) {
      PCCPatch prePatch = prePatches[patch.getBestMatchIdx()];
      if ( i == startFrameIndex ) {
        curGPAPatchData.patchOrientation = prePatch.getPatchOrientation();
        // try to place on the same position as the matched patch
        curGPAPatchData.u0 = prePatch.getU0();
        curGPAPatchData.v0 = prePatch.getV0();
      } else {
        curGPAPatchData.patchOrientation = prePatch.getCurGPAPatchData().patchOrientation;
        // try to place on the same position as the matched patch
        curGPAPatchData.u0 = prePatch.getCurGPAPatchData().u0;
        curGPAPatchData.v0 = prePatch.getCurGPAPatchData().v0;
      }
      if ( curGPAPatchData.patchOrientation == -1 ) { assert( curGPAPatchData.patchOrientation != -1 ); }

      if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                            safeguard ) ) {
        locationFound = true;
        if ( printDetailedInfo ) {
          std::cout << "Maintained TempGPA.orientation " << curGPAPatchData.patchOrientation << " for patch[" << icount
                    << "] in the same position (" << curGPAPatchData.u0 << "," << curGPAPatchData.v0 << ")"
                    << std::endl;
        }
      }

      // if the patch couldn't fit, try to fit the patch in the top left position
      for ( int v = 0; v <= occupancySizeV && !locationFound; ++v ) {
        for ( int u = 0; u <= occupancySizeU && !locationFound; ++u ) {
          curGPAPatchData.u0 = u;
          curGPAPatchData.v0 = v;
          if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV, params_.lowDelayEncoding_,
                                                safeguard ) ) {  // !!! function overload for GPA;
            locationFound = true;
            if ( printDetailedInfo ) {
              std::cout << "Maintained TempGPA.orientation " << curGPAPatchData.patchOrientation
                        << " for unmatched patch[" << icount << "] in the position (" << curGPAPatchData.u0 << ","
                        << curGPAPatchData.v0 << ")" << std::endl;
            }
          }
        }
      }
    } else {
      // best effort
      for ( size_t v = 0; v < occupancySizeV && !locationFound; ++v ) {
        for ( size_t u = 0; u < occupancySizeU && !locationFound; ++u ) {
          curGPAPatchData.u0 = u;
          curGPAPatchData.v0 = v;
          for ( size_t orientationIdx = 0; orientationIdx < numOrientations && !locationFound; orientationIdx++ ) {
            if ( patch.getSizeU0() > patch.getSizeV0() ) {
              curGPAPatchData.patchOrientation = orientation_horizontal[orientationIdx];
            } else {
              curGPAPatchData.patchOrientation = orientation_vertical[orientationIdx];
            }
            if ( patch.checkFitPatchCanvasForGPA( occupancyMap, occupancySizeU, occupancySizeV,
                                                  params_.lowDelayEncoding_, safeguard ) ) {
              locationFound = true;
              if ( printDetailedInfo ) {
                std::cout << "Maintained TempGPA.orientation " << curGPAPatchData.patchOrientation
                          << " for unmatched patch[" << icount << "] in the position (" << curGPAPatchData.u0 << ","
                          << curGPAPatchData.v0 << ")" << std::endl;
              }
            }
          }
        }
      }
    }
    if ( !locationFound ) {
      occupancySizeV *= 2;
      occupancyMap.resize( occupancySizeU * occupancySizeV );
      if ( printDetailedInfo ) { std::cout << "Increase occupancySizeV " << occupancySizeV << std::endl; }
    }
  }
  for ( size_t v0 = 0; v0 < curGPAPatchData.sizeV0; ++v0 ) {
    for ( size_t u0 = 0; u0 < curGPAPatchData.sizeU0; ++u0 ) {
      int coord = patch.patchBlock2CanvasBlockForGPA( u0, v0, occupancySizeU, occupancySizeV );
      if ( params_.lowDelayEncoding_ )
        occupancyMap[coord] = true;
      else
        occupancyMap[coord] = occupancyMap[coord] || occupancy[v0 * curGPAPatchData.sizeU0 + u0];
    }
  }
  if ( !( curGPAPatchData.isPatchDimensionSwitched() ) ) {
    heightGPA =
        ( std::max )( heightGPA, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
    widthGPA =
        ( std::max )( widthGPA, ( curGPAPatchData.u0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
    maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeV0 ) );
  } else {
    heightGPA =
        ( std::max )( heightGPA, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) * patch.getOccupancyResolution() );
    widthGPA =
        ( std::max )( widthGPA, ( curGPAPatchData.u0 + curGPAPatchData.sizeV0 ) * patch.getOccupancyResolution() );
    maxOccupancyRow = ( std::max )( maxOccupancyRow, ( curGPAPatchData.v0 + curGPAPatchData.sizeU0 ) );
  }
}

void PCCEncoder::setPointLocalReconstruction( PCCContext& context ) {
  TRACE_CODEC( "setPointLocalReconstruction \n" );  
  auto& asps = context.getAtlasSequenceParameterSet( 0 );
  TRACE_CODEC( "  MapCountMinus1 = %u \n", asps.getMapCountMinus1() );
  TRACE_CODEC( "  plrlNumberOfModes_ = %u \n", params_.plrlNumberOfModes_ );
  asps.setPointLocalReconstructionEnabledFlag( true );
  asps.allocatePointLocalReconstructionInformation();
  auto& plri = asps.getPointLocalReconstructionInformation( 0 );
  plri.setMapEnabledFlag( true );
  plri.setNumberOfModesMinus1( params_.plrlNumberOfModes_ - 1 );
  plri.setBlockThresholdPerPatchMinus1( params_.patchSize_ - 1 );
  plri.allocate();
  for ( size_t i = 0; i < plri.getNumberOfModesMinus1() + 1; i++ ) {
    auto& mode = context.getPointLocalReconstructionMode( i + 1 );
    plri.setInterpolateFlag( i, mode.interpolate_ );
    plri.setFillingFlag( i, mode.filling_ );
    plri.setMinimumDepth( i, mode.minD1_ );
    plri.setNeighbourMinus1( i, mode.neighbor_ - 1 );
  }
#ifdef CODEC_TRACE
  for ( size_t i = 0; i < context.getPointLocalReconstructionModeNumber(); i++ ) {
    auto& mode = context.getPointLocalReconstructionMode( i );
    TRACE_CODEC( "Plrm[%u]: Inter = %d Fill = %d minD1 = %u neighbor = %u \n", i, mode.interpolate_, mode.filling_,
                 mode.minD1_, mode.neighbor_ );
  }
#endif
}

void PCCEncoder::setPointLocalReconstructionData( PCCFrameContext&              frame,
                                                  const PCCPatch&               patch,
                                                  PointLocalReconstructionData& plrd,
                                                  size_t                        occupancyPackingBlockSize,
                                                  size_t                        patchIndex ) {
  plrd.allocate( patch.getSizeU0(), patch.getSizeV0() );
  const size_t blockToPatchWidth  = frame.getWidth() / params_.occupancyResolution_;
  const size_t blockToPatchHeight = frame.getHeight() / params_.occupancyResolution_;
  TRACE_CODEC( "WxH = %lu x %lu \n", plrd.getBlockToPatchMapWidth(), plrd.getBlockToPatchMapHeight() );
  plrd.setLevelFlag( patch.getPointLocalReconstructionLevel() );
  TRACE_CODEC( "  LevelFlag = %d \n", plrd.getLevelFlag() );
  if ( plrd.getLevelFlag() ) {
    plrd.setPresentFlag( patch.getPointLocalReconstructionMode() > 0 );
    plrd.setModeMinus1( patch.getPointLocalReconstructionMode() - 1 );
    TRACE_CODEC( "  ModePatch: Present = %d ModeMinus1 = %2d \n", plrd.getPresentFlag(),
                 plrd.getPresentFlag() ? (int32_t)plrd.getModeMinus1() : -1 );
  } else {
    auto& blockToPatch = frame.getBlockToPatch();
    for ( size_t v0 = 0; v0 < plrd.getBlockToPatchMapHeight(); ++v0 ) {
      for ( size_t u0 = 0; u0 < plrd.getBlockToPatchMapWidth(); ++u0 ) {
        size_t index = v0 * plrd.getBlockToPatchMapWidth() + u0;
        int    pos   = patch.patchBlock2CanvasBlock( ( u0 ), ( v0 ), blockToPatchWidth, blockToPatchHeight );
        bool   occupied =
            ( blockToPatch[pos] == patchIndex + 1 ) && ( patch.getPointLocalReconstructionMode( u0, v0 ) > 0 );
        plrd.setBlockPresentFlag( index, occupied );
        if ( occupied ) { plrd.setBlockModeMinus1( index, patch.getPointLocalReconstructionMode( u0, v0 ) - 1 ); }
        TRACE_CODEC( "  Mode[%3u]: Present = %d ModeMinus1 = %2d \n", index, plrd.getBlockPresentFlag( index ),
                     plrd.getBlockPresentFlag( index ) ? (int32_t)plrd.getBlockModeMinus1( index ) : -1 );
      }
    }
  }
#ifdef CODEC_TRACE
  for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
    for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
      TRACE_CODEC( "Block[ %2lu %2lu <=> %4lu ] / [ %2lu %2lu ]: Level = %d Present = %d mode = %lu \n", u0, v0,
                   v0 * patch.getSizeU0() + u0, patch.getSizeU0(), patch.getSizeV0(),
                   patch.getPointLocalReconstructionLevel(), plrd.getBlockPresentFlag( v0 * patch.getSizeU0() + u0 ),
                   patch.getPointLocalReconstructionMode( u0, v0 ) );
    }
  }
#endif
}

void PCCEncoder::setGeneratePointCloudParameters( GeneratePointCloudParameters& params, PCCContext& context ) {
  params.occupancyResolution_           = params_.occupancyResolution_;
  params.occupancyPrecision_            = params_.occupancyPrecision_;
  params.enableSizeQuantization_        = context.getEnablePatchSizeQuantization();
  params.flagGeometrySmoothing_         = params_.flagGeometrySmoothing_;
  params.gridSmoothing_                 = params_.gridSmoothing_;
  params.gridSize_                      = params_.gridSize_;
  params.neighborCountSmoothing_        = params_.neighborCountSmoothing_;
  params.radius2Smoothing_              = params_.radius2Smoothing_;
  params.radius2BoundaryDetection_      = params_.radius2BoundaryDetection_;
  params.thresholdSmoothing_            = params_.thresholdSmoothing_;
  params.rawPointColorFormat_           = size_t( params_.losslessGeo444_ ? COLOURFORMAT444 : COLOURFORMAT420 );
  params.nbThread_                      = params_.nbThread_;
  params.absoluteD1_                    = params_.absoluteD1_;
  params.multipleStreams_               = params_.mapCountMinus1_ == 0 || params_.multipleStreams_;
  params.surfaceThickness_              = params_.surfaceThickness_;
  params.thresholdColorSmoothing_       = params_.thresholdColorSmoothing_;
  params.thresholdColorDifference_      = params_.thresholdColorDifference_;
  params.thresholdColorVariation_       = params_.thresholdColorVariation_;
  params.thresholdLocalEntropy_         = params_.thresholdLocalEntropy_;
  params.radius2ColorSmoothing_         = params_.radius2ColorSmoothing_;
  params.neighborCountColorSmoothing_   = params_.neighborCountColorSmoothing_;
  params.flagColorSmoothing_            = params_.flagColorSmoothing_;
  params.gridColorSmoothing_            = params_.gridColorSmoothing_;
  params.cgridSize_                     = params_.cgridSize_;
  params.enhancedDeltaDepthCode_        = params_.enhancedDeltaDepthCode_;
  params.thresholdLossyOM_              = params_.thresholdLossyOM_;
  params.removeDuplicatePoints_         = params_.removeDuplicatePoints_;
  params.pointLocalReconstruction_      = params_.pointLocalReconstruction_;
  params.mapCountMinus1_                = params_.mapCountMinus1_;
  params.singleMapPixelInterleaving_    = params_.singleMapPixelInterleaving_;
  params.geometry3dCoordinatesBitdepth_ = params_.geometry3dCoordinatesBitdepth_;
  params.useAdditionalPointsPatch_      = params_.losslessGeo_ || params_.lossyMissedPointsPatch_;
  params.plrlNumberOfModes_             = params_.plrlNumberOfModes_;
  params.geometryBitDepth3D_            = params_.geometry3dCoordinatesBitdepth_;
  params.EOMFixBitCount_                = params_.EOMFixBitCount_;
  params.pbfEnableFlag_                 = false;
  params.pbfPassesCount_                = 0;
  params.pbfFilterSize_                 = 0;
  params.pbfLog2Threshold_              = 0;

  // generatePointCloudParameters.path_ = path.str();
}

void PCCEncoder::createPatchFrameDataStructure( PCCContext& context ) {
  printf( "createPatchFrameDataStructure \n" );
  TRACE_CODEC( "createPatchFrameDataStructure GOP start \n" );
  size_t frameCount = context.getFrames().size();
  TRACE_CODEC( "frameCount = %u \n", frameCount );
  TRACE_CODEC( "PLR = %d \n", params_.pointLocalReconstruction_ );

  if ( params_.pointLocalReconstruction_ ) { setPointLocalReconstruction( context ); }

  // patch reordering
  if ( params_.patchPrecedenceOrderFlag_ ) {
    std::cout << "encoder reverse ordering" << std::endl;
    for ( size_t frameIdx = 0; frameIdx < frameCount; frameIdx++ ) {
      auto&            patches = context[frameIdx].getPatches();
      vector<PCCPatch> reverseOrderPatchList;

      for ( int i = (int)patches.size() - 1; i >= 0; i-- ) {
        if ( patches[i].getBestMatchIdx() != -1 ) {
          // only 1 previous frame
          assert( frameIdx > 0 );
          patches[i].setBestMatchIdx( context[frameIdx - 1].getPatches().size() - patches[i].getBestMatchIdx() - 1 );
        }

        reverseOrderPatchList.push_back( patches[i] );
      }
      patches = reverseOrderPatchList;
    }
  }

  PCCFrameContext& refFrame = context.getFrame( 0 );
  for ( size_t i = 0; i < frameCount; i++ ) {
    //*****//
    PCCFrameContext& frame = context.getFrame( i );
    createPatchFrameDataStructure( context, frame, i );
    //*****//
    AtlasTileGroupHeader& atgh         = context.getAtlasTileGroupLayer( i ).getAtlasTileGroupHeader();
    size_t                afpsIdInList = 0;
    for ( afpsIdInList = 0; afpsIdInList < context.getAtlasFrameParameterSetList().size(); afpsIdInList++ ) {
      auto& afpsInList = context.getAtlasFrameParameterSet( afpsIdInList );
      if ( ( frame.get2dPosXBitCountMinus1() <= afpsInList.getAfps2dPosXBitCountMinus1() ) &&
           ( frame.get2dPosYBitCountMinus1() <= afpsInList.getAfps2dPosYBitCountMinus1() ) &&
           ( frame.get3dPosXBitCountMinus1() <= afpsInList.getAfps3dPosXBitCountMinus1() ) &&
           ( frame.get3dPosYBitCountMinus1() <= afpsInList.getAfps3dPosYBitCountMinus1() ) ) {
        atgh.setAtghAtlasFrameParameterSetId( afpsInList.getAtlasFrameParameterSetId() );
        break;
      }
    }  // list
    if ( afpsIdInList == context.getAtlasFrameParameterSetList().size() ) {
      size_t afpsId = context.addAtlasFrameParameterSet( context.getAtlasFrameParameterSet( 0 ) );
      atgh.setAtghAtlasFrameParameterSetId( afpsId );
      auto& afps = context.getAtlasFrameParameterSet( afpsId );
      afps.setAfps2dPosXBitCountMinus1( frame.get2dPosXBitCountMinus1() );
      afps.setAfps2dPosYBitCountMinus1( frame.get2dPosYBitCountMinus1() );
      afps.setAfps3dPosXBitCountMinus1( frame.get3dPosXBitCountMinus1() );
      afps.setAfps3dPosYBitCountMinus1( frame.get3dPosYBitCountMinus1() );
    }
  }
  if ( params_.flagGeometrySmoothing_ || params_.flagColorSmoothing_ ) {
    SEISmoothingParameters& sei = static_cast<SEISmoothingParameters&>( context.addSeiPrefix( SMOOTHING_PARAMETERS ) );
    if ( params_.flagGeometrySmoothing_ ) {
      sei.setSpGeometryCancelFlag( false );
      sei.setSpGeometrySmoothingEnabledFlag( params_.gridSmoothing_ || params_.pbfEnableFlag_ );
      if ( params_.gridSmoothing_ ) {
        sei.setSpGeometrySmoothingId( 0 );
        sei.setSpGeometrySmoothingGridSizeMinus2( params_.gridSize_ - 2 );
        sei.setSpGeometrySmoothingThreshold( params_.thresholdSmoothing_ );
      }
      if ( params_.pbfEnableFlag_ ) {
        sei.setSpGeometrySmoothingId( 1 );
        sei.setSpGeometryPatchBlockFilteringLog2ThresholdMinus1( params_.pbfLog2Threshold_ - 1 );
        sei.setSpGeometryPatchBlockFilteringPassesCountMinus1( params_.pbfPassesCount_ - 1 );
        sei.setSpGeometryPatchBlockFilteringFilterSizeMinus1( params_.pbfFilterSize_ - 1 );
      }
    }
    if ( params_.flagColorSmoothing_ ) {
      sei.setSpAttributeCancelFlag( false );
      sei.setSpNumAttributeUpdates( 1 );
      sei.allocate();
      for ( size_t j = 0; j < sei.getSpNumAttributeUpdates(); j++ ) {
        sei.setSpAttributeIdx( j, 0 );
        size_t index     = sei.getSpAttributeIdx( j );
        size_t dimention = 3;
        sei.allocate( index + 1, dimention + 1 );
        sei.setSpDimensionMinus1( index, dimention + 1 );
        for ( size_t i = 0; i < sei.getSpDimensionMinus1( index ) + 1; i++ ) {
          sei.setSpAttrSmoothingParamsEnabledFlag( index, i, true );
          if ( sei.getSpAttrSmoothingParamsEnabledFlag( index, i ) ) {
            sei.setSpAttrSmoothingGridSizeMinus2( index, i, params_.gridColorSmoothing_ - 2 );
            sei.setSpAttrSmoothingThreshold( index, i, params_.thresholdColorSmoothing_ );
            sei.setSpAttrSmoothingLocalEntropyThreshold( index, i, params_.thresholdLocalEntropy_ );
            sei.setSpAttrSmoothingThresholdVariation( index, i, params_.thresholdColorVariation_ );
            sei.setSpAttrSmoothingThresholdDifference( index, i, params_.thresholdColorDifference_ );
          }
        }
      }
    }
  }
}

void PCCEncoder::createPatchFrameDataStructure( PCCContext& context, PCCFrameContext& frame, size_t frameIndex ) {
  TRACE_CODEC( "createPatchFrameDataStructure Frame %lu \n", frame.getIndex() );
  auto&        patches                   = frame.getPatches();
  auto&        pcmPatches                = frame.getMissedPointsPatches();
  auto&        sps                       = context.getSps();
  auto&        atglu                     = context.getAtlasTileGroupLayer( frameIndex );
  auto&        atgh                      = atglu.getAtlasTileGroupHeader();
  auto&        atgdu                     = atglu.getAtlasTileGroupDataUnit();
  size_t       afpsId                    = 0;
  auto&        afps                      = context.getAtlasFrameParameterSet( afpsId );
  size_t       aspsId                    = afps.getAtlasSequenceParameterSetId();
  auto&        asps                      = context.getAtlasSequenceParameterSet( aspsId );
  const size_t minLevel                  = sps.getMinLevel();
  size_t       atlasIndex                = 0;
  auto&        gi                        = sps.getGeometryInformation( atlasIndex );
  auto         geometryBitDepth2D        = gi.getGeometryNominal2dBitdepthMinus1() + 1;
  uint8_t      maxBitCountForMaxDepthTmp = uint8_t( geometryBitDepth2D - gbitCountSize[minLevel] + 1 );
  uint8_t      maxBitCountForMinDepthTmp = uint8_t( 10 - gbitCountSize[minLevel] );
  if ( asps.get45DegreeProjectionPatchPresentFlag() ) {
    maxBitCountForMaxDepthTmp += 1;
    maxBitCountForMinDepthTmp += 1;
  }
  int64_t prevSizeU0 = 0, prevSizeV0 = 0, predIndex = 0;
  TRACE_CODEC( "Patches size                        = %lu \n", patches.size() );
  TRACE_CODEC( "non-regular Patches(pcm, eom)     = %lu, %lu \n", frame.getMissedPointsPatches().size(),
               frame.getEomPatches().size() );
  atglu.setFrameIndex( frameIndex );
  atgh.setAtghAtlasFrmOrderCntLsb( frameIndex );
  atgh.setAtghType( I_TILE_GRP );  // P_TILE_GRP = 0, SKIP_TILE_GRP, I_TILE_GRP
  if ( frameIndex != 0 ) {         // && sps.getPatchInterPredictionEnabledFlag() ){
    bool interPredPresent = false;
    for ( auto& patch : patches ) {
      interPredPresent |= ( patch.getBestMatchIdx() != InvalidPatchIndex );
      if ( interPredPresent ) break;
    }
    if ( interPredPresent ) { atgh.setAtghType( P_TILE_GRP ); }
  }
  if ( atgh.getAtghType() == I_TILE_GRP ) {
    for ( auto& patch : patches ) patch.setBestMatchIdx( InvalidPatchIndex );
    frame.setNumMatchedPatches( 0 );
  }
  frame.constructAtghRefListStruct( context, atgh );
  TRACE_CODEC( "TileGroup Type                     = %zu (0.P_TILE_GRP 1.SKIP_TILE_GRP 2.I_TILE_GRP)\n",
               (size_t)atgh.getAtghType() );
  TRACE_CODEC( "OccupancyPackingBlockSize           = %d \n", context.getOccupancyPackingBlockSize() );

  // all patches
  size_t  totalPatchCount = patches.size() + frame.getMissedPointsPatches().size() + frame.getEomPatches().size();
  int32_t quantizerSizeX  = 1 << params_.log2QuantizerSizeX_;
  int32_t quantizerSizeY  = 1 << params_.log2QuantizerSizeY_;
  for ( size_t patchIndex = 0; patchIndex < patches.size(); patchIndex++ ) {
    const auto& patch = patches[patchIndex];
    if ( patch.getBestMatchIdx() != InvalidPatchIndex ) {
      // INTER patches
      size_t      refPOC   = frame.getRefAFOC( patch.getRefAtlasFrameIndex() );
      const auto& refPatch = context.getFrame( refPOC ).getPatches()[patch.getBestMatchIdx()];
      auto&       pid      = atgdu.addPatchInformationData( (uint8_t)PATCH_MODE_P_INTER );
      TRACE_CODEC( "patch %lu / %lu: Inter \n", patchIndex, totalPatchCount );
      auto& ipdu = pid.getInterPatchDataUnit();
      ipdu.setIpduRefIndex( patch.getRefAtlasFrameIndex() );
      ipdu.setIpduRefPatchIndex( (int64_t)patch.getBestMatchIdx() - predIndex );
      ipdu.setIpdu2dPosX( patch.getU0() - refPatch.getU0() );
      ipdu.setIpdu2dPosY( patch.getV0() - refPatch.getV0() );
      if ( asps.getPatchSizeQuantizerPresentFlag() ) {
        int32_t deltaSizeX =
            patch.getPatchSize2DXInPixel() / quantizerSizeX - refPatch.getPatchSize2DXInPixel() / quantizerSizeX;
        int32_t deltaSizeY =
            patch.getPatchSize2DYInPixel() / quantizerSizeY - refPatch.getPatchSize2DYInPixel() / quantizerSizeY;
        ipdu.setIpdu2dDeltaSizeX( deltaSizeX );
        ipdu.setIpdu2dDeltaSizeY( deltaSizeY );
      } else {
        ipdu.setIpdu2dDeltaSizeX( patch.getSizeU0() - refPatch.getSizeU0() );
        ipdu.setIpdu2dDeltaSizeY( patch.getSizeV0() - refPatch.getSizeV0() );
      }
      ipdu.setIpdu3dPosX( patch.getU1() - refPatch.getU1() );
      ipdu.setIpdu3dPosY( patch.getV1() - refPatch.getV1() );

      const size_t max3DCoordinate = 1 << ( gi.getGeometry3dCoordinatesBitdepthMinus1() + 1 );
      if ( patch.getProjectionMode() == 0 ) {
        ipdu.setIpdu3dPosMinZ( ( patch.getD1() / minLevel ) - ( refPatch.getD1() / minLevel ) );
      } else {
        if ( asps.get45DegreeProjectionPatchPresentFlag() == 0 ) {
          ipdu.setIpdu3dPosMinZ( ( max3DCoordinate - patch.getD1() ) / minLevel -
                                 ( max3DCoordinate - refPatch.getD1() ) / minLevel );
        } else {
          ipdu.setIpdu3dPosMinZ( ( ( max3DCoordinate << 1 ) - patch.getD1() ) / minLevel -
                                 ( ( max3DCoordinate << 1 ) - refPatch.getD1() ) / minLevel );
        }
      }

      size_t        quantDD  = patch.getSizeD() == 0 ? 0 : ( ( patch.getSizeD() - 1 ) / minLevel + 1 );
      size_t        prevQDD  = refPatch.getSizeD() == 0 ? 0 : ( ( refPatch.getSizeD() - 1 ) / minLevel + 1 );
      const int64_t delta_dd = ( (int64_t)quantDD ) - ( (int64_t)prevQDD );
      ipdu.setIpdu3dPosDeltaMaxZ( delta_dd );
      TRACE_CODEC(
          "IPDU: refAtlasFrame= %d refPatchIdx = %d pos2DXY = %ld %ld pos3DXYZW = %ld %ld %ld %ld size2D = %ld %ld \n",
          ipdu.getIpduRefIndex(), ipdu.getIpduRefPatchIndex(), ipdu.getIpdu2dPosX(), ipdu.getIpdu2dPosY(),
          ipdu.getIpdu3dPosX(), ipdu.getIpdu3dPosY(), ipdu.getIpdu3dPosMinZ(), ipdu.getIpdu3dPosDeltaMaxZ(),
          ipdu.getIpdu2dDeltaSizeX(), ipdu.getIpdu2dDeltaSizeY() );
      TRACE_CODEC( "\trefPatch: Idx = %lu UV0 = %lu %lu  UV1 = %lu %lu Size = %lu %lu %lu  Lod = %u,%u\n",
                   patch.getBestMatchIdx(), refPatch.getU0(), refPatch.getV0(), refPatch.getU1(), refPatch.getV1(),
                   refPatch.getSizeU0(), refPatch.getSizeV0(), refPatch.getSizeD(), refPatch.getLodScaleX(),
                   refPatch.getLodScaleY() );

      if ( asps.getPointLocalReconstructionEnabledFlag() ) {
        setPointLocalReconstructionData( frame, patch, ipdu.getPointLocalReconstructionData(),
                                         context.getOccupancyPackingBlockSize(), patchIndex );
      }
      prevSizeU0 = asps.getPatchSizeQuantizerPresentFlag() ? patch.getPatchSize2DXInPixel()
                                                           : patch.getSizeU0();  // prevPatchSize2DXInPixel
      prevSizeV0 = asps.getPatchSizeQuantizerPresentFlag() ? patch.getPatchSize2DYInPixel() : patch.getSizeV0();
      predIndex += ipdu.getIpduRefPatchIndex() + 1;
      TRACE_CODEC(
          "patch(Inter) %zu: UV0 %4lu %4lu UV1 %4lu %4lu D1=%4lu S=%4lu %4lu %4lu from DeltaSize = "
          "%4ld %4ld P=%lu O=%lu A=%u%u%u Lod = %lu,%lu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), ipdu.getIpdu2dDeltaSizeX(), ipdu.getIpdu2dDeltaSizeY(),
          patch.getProjectionMode(), patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(),
          patch.getBitangentAxis(), patch.getLodScaleX(), patch.getLodScaleY() );

    } else {
      // INTRA patches
      uint8_t patchType =
          ( atgh.getAtghType() == I_TILE_GRP ) ? (uint8_t)PATCH_MODE_I_INTRA : (uint8_t)PATCH_MODE_P_INTRA;
      auto& pid = atgdu.addPatchInformationData( patchType );
      TRACE_CODEC( "patch %lu / %lu: Intra \n", patchIndex, totalPatchCount );
      auto& pdu = pid.getPatchDataUnit();
      pdu.setPdu2dPosX( patch.getU0() );
      pdu.setPdu2dPosY( patch.getV0() );
      bool lodEnableFlag = ( patch.getLodScaleX() > 1 || patch.getLodScaleY() > 1 );
      // PatchLoDScaleX[ p ] = pdu_lod_enable_flag[ p ] ? pdu_lod_scale_x_minus1[ p ] + 1: 1
      // PatchLoDScaleY[ p ] = pdu_lod_enable_flag[ p ] ? (pdu_lod_scale_y[ p ] + (pdu_lod_scale_x_minus1[ p ] > 0) ? 1
      // : 2) : 1
      if ( afps.getLodModeEnableFlag() ) {
        pdu.setLodEnableFlag( lodEnableFlag );
        if ( lodEnableFlag ) {
          pdu.setLodScaleXminus1( patch.getLodScaleX() - 1 );
          pdu.setLodScaleY( patch.getLodScaleY() - ( patch.getLodScaleX() > 1 ? 1 : 2 ) );
        }
      } else {
        pdu.setLodEnableFlag( 0 );
        pdu.setLodScaleXminus1( 0 );
        pdu.setLodScaleY( 0 );
      }

      pdu.setPdu3dPosX( patch.getU1() );
      pdu.setPdu3dPosY( patch.getV1() );
      size_t pduProjectPlane = patch.getProjectionMode() * 3 + size_t( patch.getNormalAxis() );
      pdu.setPduProjectionId( asps.get45DegreeProjectionPatchPresentFlag() ? ( pduProjectPlane << 2 )
                                                                           : pduProjectPlane );
      if ( asps.getPatchSizeQuantizerPresentFlag() ) {
        int32_t deltaSizeX = patch.getPatchSize2DXInPixel() / quantizerSizeX - prevSizeU0 / quantizerSizeX;
        int32_t deltaSizeY = patch.getPatchSize2DYInPixel() / quantizerSizeY - prevSizeV0 / quantizerSizeY;
        pdu.setPdu2dDeltaSizeX( deltaSizeX );
        pdu.setPdu2dDeltaSizeY( deltaSizeY );
      } else {
        pdu.setPdu2dDeltaSizeX( patch.getSizeU0() - prevSizeU0 );
        pdu.setPdu2dDeltaSizeY( patch.getSizeV0() - prevSizeV0 );
      }
      pdu.setPduOrientationIndex( patch.getPatchOrientation() );
      const size_t max3DCoordinate = 1 << ( gi.getGeometry3dCoordinatesBitdepthMinus1() + 1 );
      if ( patch.getProjectionMode() == 0 ) {
        pdu.setPdu3dPosMinZ( patch.getD1() / minLevel );
      } else {
        if ( asps.get45DegreeProjectionPatchPresentFlag() == 0 ) {
          pdu.setPdu3dPosMinZ( ( max3DCoordinate - patch.getD1() ) / minLevel );
        } else {
          pdu.setPdu3dPosMinZ( ( ( max3DCoordinate << 1 ) - patch.getD1() ) / minLevel );
        }
      }
      prevSizeU0     = asps.getPatchSizeQuantizerPresentFlag() ? patch.getPatchSize2DXInPixel() : patch.getSizeU0();
      prevSizeV0     = asps.getPatchSizeQuantizerPresentFlag() ? patch.getPatchSize2DYInPixel() : patch.getSizeV0();
      size_t quantDD = patch.getSizeD() == 0 ? 0 : ( ( patch.getSizeD() - 1 ) / minLevel + 1 );
      pdu.setPdu3dPosDeltaMaxZ( quantDD );
      TRACE_CODEC(
          "patch(Intra) %zu: UV0 %4lu %4lu UV1 %4lu %4lu D1=%4lu S=%4lu %4lu %4lu(%4lu) P=%lu O=%lu A=%u%u%u Lod "
          "=(%zu) "
          "%lu,%lu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), pdu.getPdu3dPosDeltaMaxZ(), patch.getProjectionMode(),
          patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(), patch.getBitangentAxis(),
          (size_t)lodEnableFlag, patch.getLodScaleX(), patch.getLodScaleY() );

      if ( asps.getPointLocalReconstructionEnabledFlag() ) {
        setPointLocalReconstructionData( frame, patch, pdu.getPointLocalReconstructionData(),
                                         context.getOccupancyPackingBlockSize(), patchIndex );
      }
    }
  }

  if ( ( sps.getLosslessGeo() || params_.lossyMissedPointsPatch_ ) ) {
    size_t numberOfPcmPatches = frame.getNumberOfMissedPointsPatches();
    for ( size_t mpsPatchIndex = 0; mpsPatchIndex < numberOfPcmPatches; ++mpsPatchIndex ) {
      auto&   missedPointsPatch = pcmPatches[mpsPatchIndex];
      uint8_t patchType = ( atgh.getAtghType() == I_TILE_GRP ) ? (uint8_t)PATCH_MODE_I_RAW : (uint8_t)PATCH_MODE_P_RAW;
      auto& pid  = atgdu.addPatchInformationData( patchType );
      auto& ppdu = pid.getRawPatchDataUnit();
      TRACE_CODEC( "patch %lu / %lu: raw \n", patches.size() + mpsPatchIndex, totalPatchCount );
      ppdu.setRpdu2dPosX( missedPointsPatch.u0_ );
      ppdu.setRpdu2dPosY( missedPointsPatch.v0_ );
      ppdu.setRpdu2dDeltaSizeX( missedPointsPatch.sizeU0_ );
      ppdu.setRpdu2dDeltaSizeY( missedPointsPatch.sizeV0_ );
      if ( afps.getAfpsRaw3dPosBitCountExplicitModeFlag() ) {
        ppdu.setRpdu3dPosX( missedPointsPatch.u1_ );
        ppdu.setRpdu3dPosY( missedPointsPatch.v1_ );
        ppdu.setRpdu3dPosZ( missedPointsPatch.d1_ );
      } else {
        const size_t pcmU1V1D1Level = 1 << ( gi.getGeometryNominal2dBitdepthMinus1() + 1 );
        ppdu.setRpdu3dPosX( missedPointsPatch.u1_ / pcmU1V1D1Level );
        ppdu.setRpdu3dPosY( missedPointsPatch.v1_ / pcmU1V1D1Level );
        ppdu.setRpdu3dPosZ( missedPointsPatch.d1_ / pcmU1V1D1Level );
      }
      ppdu.setRpduPatchInRawVideoFlag( sps.getRawSeparateVideoPresentFlag( 0 ) );
      ppdu.setRpduRawPoints( uint32_t( missedPointsPatch.getNumberOfMps() ) );
      TRACE_CODEC( "Raw :UV = %lu %lu  size = %lu %lu  uvd1 = %lu %lu %lu numPoints = %lu ocmRes = %lu \n",
                   missedPointsPatch.u0_, missedPointsPatch.v0_, missedPointsPatch.sizeU0_, missedPointsPatch.sizeV0_,
                   missedPointsPatch.u1_, missedPointsPatch.v1_, missedPointsPatch.d1_,
                   missedPointsPatch.getNumberOfMps(), missedPointsPatch.occupancyResolution_ );
    }
  }
  if ( params_.enhancedDeltaDepthCode_ ) {
    size_t numberOfEomPatches = frame.getEomPatches().size();
    for ( size_t eomPatchIndex = 0; eomPatchIndex < numberOfEomPatches; ++eomPatchIndex ) {
      auto&   eomPatch  = frame.getEomPatches()[eomPatchIndex];
      uint8_t patchType = ( atgh.getAtghType() == I_TILE_GRP ) ? (uint8_t)PATCH_MODE_I_EOM : (uint8_t)PATCH_MODE_P_EOM;
      auto&   pid       = atgdu.addPatchInformationData( patchType );
      auto&   epdu      = pid.getEomPatchDataUnit();
      TRACE_CODEC( "patch %lu / %lu: EOM \n", patches.size() + pcmPatches.size() + eomPatchIndex, totalPatchCount );
      epdu.setEpdu2dPosX( eomPatch.u0_ );
      epdu.setEpdu2dPosY( eomPatch.v0_ );
      epdu.setEpdu2dDeltaSizeX( eomPatch.sizeU_ );
      epdu.setEpdu2dDeltaSizeY( eomPatch.sizeV_ );
      assert( eomPatch.memberPatches.size() >= 1 );
      epdu.setEpduAssociatedPatchesCountMinus1( uint32_t( eomPatch.memberPatches.size() - 1 ) );
      for ( size_t i = 0; i < eomPatch.memberPatches.size(); i++ ) {
        epdu.setEpduAssociatedPatches( eomPatch.memberPatches[i], i );
        epdu.setEpduEomPointsPerPatch( eomPatch.eddCountPerPatch[i], i );
      }
      TRACE_CODEC( "EOM: U0V0 %lu,%lu\tSizeU0V0 %lu,%lu\tN= %lu,%lu\n", eomPatch.u0_, eomPatch.v0_, eomPatch.sizeU_,
                   eomPatch.sizeV_, eomPatch.memberPatches.size(), eomPatch.eddCount_ );
      for ( size_t i = 0; i < eomPatch.memberPatches.size(); i++ ) {
        TRACE_CODEC( "%lu, %lu\n", eomPatch.memberPatches[i], eomPatch.eddCountPerPatch[i] );
      }
    }
  }
  TRACE_CODEC( "patch %lu / %lu: end \n", patches.size(), patches.size() );
  uint8_t patchType = ( atgh.getAtghType() == I_TILE_GRP ) ? (uint8_t)PATCH_MODE_I_END : (uint8_t)PATCH_MODE_P_END;
  atgdu.addPatchInformationData( patchType );
  size_t maxU0 = 0, maxV0 = 0, maxU1 = 0, maxV1 = 0;
  for ( size_t patchIndex = 0; patchIndex < patches.size(); ++patchIndex ) {
    const auto& patch = patches[patchIndex];
    if ( patch.getBestMatchIdx() == InvalidPatchIndex ) {
      maxU0 = ( std::max )( maxU0, patch.getU0() );
      maxV0 = ( std::max )( maxV0, patch.getV0() );
      maxU1 = ( std::max )( maxU1, patch.getU1() );
      maxV1 = ( std::max )( maxV1, patch.getV1() );
    }
  }
  if ( ( sps.getLosslessGeo() || params_.lossyMissedPointsPatch_ ) ) {
    auto&  pcmPatches         = frame.getMissedPointsPatches();
    size_t numberOfMpsPatches = frame.getNumberOfMissedPointsPatches();
    for ( size_t mpsPatchIndex = 0; mpsPatchIndex < numberOfMpsPatches; ++mpsPatchIndex ) {
      const auto& pcmPatch = pcmPatches[mpsPatchIndex];
      maxU0                = ( std::max )( maxU0, pcmPatch.u0_ );
      maxV0                = ( std::max )( maxV0, pcmPatch.v0_ );
      maxU1                = ( std::max )( maxU1, pcmPatch.u1_ );
      maxV1                = ( std::max )( maxV1, pcmPatch.v1_ );
    }
  }
  if ( params_.enhancedDeltaDepthCode_ ) {
    for ( auto& eomPatch : frame.getEomPatches() ) {
      maxU0 = ( std::max )( maxU0, eomPatch.u0_ );
      maxV0 = ( std::max )( maxV0, eomPatch.v0_ );
    }
  }
  const uint8_t bitCountU0 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxU0 + 1 ) ) );
  const uint8_t bitCountV0 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxV0 + 1 ) ) );
  const uint8_t bitCountU1 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxU1 + 1 ) ) );
  const uint8_t bitCountV1 = uint8_t( getFixedLengthCodeBitsCount( uint32_t( maxV1 + 1 ) ) );

  frame.set2dPosXBitCountMinus1( bitCountU0 > 0 ? ( bitCountU0 - 1 ) : 0 );
  frame.set2dPosYBitCountMinus1( bitCountV0 > 0 ? ( bitCountV0 - 1 ) : 0 );
  frame.set3dPosXBitCountMinus1( bitCountU1 > 0 ? ( bitCountU1 - 1 ) : 0 );
  frame.set3dPosYBitCountMinus1( bitCountV1 > 0 ? ( bitCountV1 - 1 ) : 0 );
  if ( frameIndex == 0 ) {
    afps.setAfps2dPosXBitCountMinus1( frame.get2dPosXBitCountMinus1() );
    afps.setAfps2dPosYBitCountMinus1( frame.get2dPosYBitCountMinus1() );
    afps.setAfps3dPosXBitCountMinus1( frame.get3dPosXBitCountMinus1() );
    afps.setAfps3dPosYBitCountMinus1( frame.get3dPosYBitCountMinus1() );
  }
}

void PCCEncoder::SegmentationPartiallyAddtinalProjectionPlane( const PCCPointSet3&                source,
                                                               PCCFrameContext&                   frame,
                                                               const PCCPatchSegmenter3Parameters segmenterParams,
                                                               PCCVideoGeometry&                  videoGeometry,
                                                               PCCFrameContext&                   prevFrame,
                                                               size_t                             frameIndex,
                                                               float&                             distanceSrcRec ) {
  std::vector<PCCPatch> Orthogonal;
  std::vector<PCCPatch> Additional;

  size_t axis  = 0;
  int    min_x = ( 1u << segmenterParams.geometryBitDepth3D_ ) + 1;
  int    max_x = -1;
  int    min_y = min_x;
  int    max_y = -1;
  int    min_z = min_x;
  int    max_z = -1;

  for ( size_t i = 0; i < source.getPointCount(); i++ ) {
    PCCPoint3D point = source[i];
    if ( min_x > point.x() ) { min_x = point.x(); }
    if ( min_y > point.y() ) { min_y = point.y(); }
    if ( min_z > point.z() ) { min_z = point.z(); }
    if ( max_x < point.x() ) { max_x = point.x(); }
    if ( max_y < point.y() ) { max_y = point.y(); }
    if ( max_z < point.z() ) { max_z = point.z(); }
  }
  int Id = 0;
  if ( max_x - min_x > max_y - min_y ) {
    Id = 1;
  } else {
    Id = 2;
  }
  if ( Id == 1 && max_z - min_z > max_x - min_x ) { Id = 3; }
  if ( Id == 2 && max_z - min_z > max_y - min_y ) { Id = 3; }

  // Id is 1:X, Id is 2:Y Id is 3:Z
  axis = Id;
  PCCPointSet3 partial;
  partial.clear();
  partial.addColors();

  double ratio = 1.0 - params_.partialAdditionalProjectionPlane_;
  for ( size_t i = 0; i < source.getPointCount(); i++ ) {
    PCCPoint3D point = source[i];
    PCCColor3B color = source.getColor( i );  // finally recolor color was used.

    if ( axis == 1 ) {
      if ( point.x() > min_x + ( max_x - min_x ) * ratio ) {
        PCCVector3D pos;
        pos.x() = point.x();
        pos.y() = point.y();
        pos.z() = point.z();
        partial.addPoint( pos, color );
      }
    }
    if ( axis == 2 ) {
      if ( point.y() > min_y + ( max_y - min_y ) * ratio ) {
        PCCVector3D pos;
        pos.x() = point.x();
        pos.y() = point.y();
        pos.z() = point.z();
        partial.addPoint( pos, color );
      }
    }
    if ( axis == 3 ) {
      if ( point.z() > min_z + ( max_z - min_z ) * ratio ) {
        PCCVector3D pos;
        pos.x() = point.x();
        pos.y() = point.y();
        pos.z() = point.z();
        partial.addPoint( pos, color );
      }
    }
  }

  {  // orthogonal 6 projection
    std::vector<PCCPointSet3>    Tmp;
    std::vector<PCCPointSet3>    PointCloudByPatchA;
    PCCPointSet3                 resampleKeepA;
    PCCPatchSegmenter3Parameters local   = segmenterParams;
    local.additionalProjectionPlaneMode_ = 0;
    PCCPatchSegmenter3 segmenter;
    Orthogonal.reserve( 256 );
    float distanceSrcRecA;
    segmenter.setNbThread( params_.nbThread_ );
    segmenter.compute( source, frame.getIndex(), local, Orthogonal, frame.getSrcPointCloudByPatch(), distanceSrcRecA );
    distanceSrcRec                  = distanceSrcRecA;
    frame.getSrcPointCloudByPatch() = Tmp;
  }

  if ( partial.getPointCount() ) {
    // additional projection
    std::vector<PCCPointSet3>    Tmp;
    std::vector<PCCPointSet3>    PointCloudByPatchA;
    PCCPointSet3                 resampleKeepA;
    PCCPatchSegmenter3Parameters local = segmenterParams;

    if ( axis == 1 ) { local.additionalProjectionPlaneMode_ = 2; }
    if ( axis == 2 ) { local.additionalProjectionPlaneMode_ = 1; }
    if ( axis == 3 ) { local.additionalProjectionPlaneMode_ = 3; }

    PCCPatchSegmenter3 segmenter;
    Additional.reserve( 256 );
    float distanceSrcRecA;
    segmenter.setNbThread( params_.nbThread_ );
    segmenter.compute( partial, frame.getIndex(), local, Additional, frame.getSrcPointCloudByPatch(), distanceSrcRecA );
    distanceSrcRec                  = distanceSrcRecA;
    frame.getSrcPointCloudByPatch() = Tmp;

    // remove
    const size_t        patchCount = Additional.size();
    int                 patchIndex;
    std::vector<size_t> remove;
    remove.clear();
    for ( patchIndex = patchCount - 1; patchIndex > -1; --patchIndex ) {
      auto& patch = Additional[patchIndex];
      if ( patch.getAxisOfAdditionalPlane() == 0 ) { remove.push_back( patchIndex ); }
    }
    // erace
    for ( auto itr = remove.begin(); itr != remove.end(); ++itr ) { Additional.erase( Additional.begin() + *itr ); }
  }
  auto& patches = frame.getPatches();
  patches.reserve( Orthogonal.size() + Additional.size() );
  std::copy( Orthogonal.begin(), Orthogonal.end(), std::back_inserter( patches ) );
  std::copy( Additional.begin(), Additional.end(), std::back_inserter( patches ) );
}

// Morton
const uint32_t kMortonCode256Z[256] = {
    0x00000000, 0x00000001, 0x00000008, 0x00000009, 0x00000040, 0x00000041, 0x00000048, 0x00000049, 0x00000200,
    0x00000201, 0x00000208, 0x00000209, 0x00000240, 0x00000241, 0x00000248, 0x00000249, 0x00001000, 0x00001001,
    0x00001008, 0x00001009, 0x00001040, 0x00001041, 0x00001048, 0x00001049, 0x00001200, 0x00001201, 0x00001208,
    0x00001209, 0x00001240, 0x00001241, 0x00001248, 0x00001249, 0x00008000, 0x00008001, 0x00008008, 0x00008009,
    0x00008040, 0x00008041, 0x00008048, 0x00008049, 0x00008200, 0x00008201, 0x00008208, 0x00008209, 0x00008240,
    0x00008241, 0x00008248, 0x00008249, 0x00009000, 0x00009001, 0x00009008, 0x00009009, 0x00009040, 0x00009041,
    0x00009048, 0x00009049, 0x00009200, 0x00009201, 0x00009208, 0x00009209, 0x00009240, 0x00009241, 0x00009248,
    0x00009249, 0x00040000, 0x00040001, 0x00040008, 0x00040009, 0x00040040, 0x00040041, 0x00040048, 0x00040049,
    0x00040200, 0x00040201, 0x00040208, 0x00040209, 0x00040240, 0x00040241, 0x00040248, 0x00040249, 0x00041000,
    0x00041001, 0x00041008, 0x00041009, 0x00041040, 0x00041041, 0x00041048, 0x00041049, 0x00041200, 0x00041201,
    0x00041208, 0x00041209, 0x00041240, 0x00041241, 0x00041248, 0x00041249, 0x00048000, 0x00048001, 0x00048008,
    0x00048009, 0x00048040, 0x00048041, 0x00048048, 0x00048049, 0x00048200, 0x00048201, 0x00048208, 0x00048209,
    0x00048240, 0x00048241, 0x00048248, 0x00048249, 0x00049000, 0x00049001, 0x00049008, 0x00049009, 0x00049040,
    0x00049041, 0x00049048, 0x00049049, 0x00049200, 0x00049201, 0x00049208, 0x00049209, 0x00049240, 0x00049241,
    0x00049248, 0x00049249, 0x00200000, 0x00200001, 0x00200008, 0x00200009, 0x00200040, 0x00200041, 0x00200048,
    0x00200049, 0x00200200, 0x00200201, 0x00200208, 0x00200209, 0x00200240, 0x00200241, 0x00200248, 0x00200249,
    0x00201000, 0x00201001, 0x00201008, 0x00201009, 0x00201040, 0x00201041, 0x00201048, 0x00201049, 0x00201200,
    0x00201201, 0x00201208, 0x00201209, 0x00201240, 0x00201241, 0x00201248, 0x00201249, 0x00208000, 0x00208001,
    0x00208008, 0x00208009, 0x00208040, 0x00208041, 0x00208048, 0x00208049, 0x00208200, 0x00208201, 0x00208208,
    0x00208209, 0x00208240, 0x00208241, 0x00208248, 0x00208249, 0x00209000, 0x00209001, 0x00209008, 0x00209009,
    0x00209040, 0x00209041, 0x00209048, 0x00209049, 0x00209200, 0x00209201, 0x00209208, 0x00209209, 0x00209240,
    0x00209241, 0x00209248, 0x00209249, 0x00240000, 0x00240001, 0x00240008, 0x00240009, 0x00240040, 0x00240041,
    0x00240048, 0x00240049, 0x00240200, 0x00240201, 0x00240208, 0x00240209, 0x00240240, 0x00240241, 0x00240248,
    0x00240249, 0x00241000, 0x00241001, 0x00241008, 0x00241009, 0x00241040, 0x00241041, 0x00241048, 0x00241049,
    0x00241200, 0x00241201, 0x00241208, 0x00241209, 0x00241240, 0x00241241, 0x00241248, 0x00241249, 0x00248000,
    0x00248001, 0x00248008, 0x00248009, 0x00248040, 0x00248041, 0x00248048, 0x00248049, 0x00248200, 0x00248201,
    0x00248208, 0x00248209, 0x00248240, 0x00248241, 0x00248248, 0x00248249, 0x00249000, 0x00249001, 0x00249008,
    0x00249009, 0x00249040, 0x00249041, 0x00249048, 0x00249049, 0x00249200, 0x00249201, 0x00249208, 0x00249209,
    0x00249240, 0x00249241, 0x00249248, 0x00249249};

const uint32_t kMortonCode256Y[256] = {
    0x00000000, 0x00000002, 0x00000010, 0x00000012, 0x00000080, 0x00000082, 0x00000090, 0x00000092, 0x00000400,
    0x00000402, 0x00000410, 0x00000412, 0x00000480, 0x00000482, 0x00000490, 0x00000492, 0x00002000, 0x00002002,
    0x00002010, 0x00002012, 0x00002080, 0x00002082, 0x00002090, 0x00002092, 0x00002400, 0x00002402, 0x00002410,
    0x00002412, 0x00002480, 0x00002482, 0x00002490, 0x00002492, 0x00010000, 0x00010002, 0x00010010, 0x00010012,
    0x00010080, 0x00010082, 0x00010090, 0x00010092, 0x00010400, 0x00010402, 0x00010410, 0x00010412, 0x00010480,
    0x00010482, 0x00010490, 0x00010492, 0x00012000, 0x00012002, 0x00012010, 0x00012012, 0x00012080, 0x00012082,
    0x00012090, 0x00012092, 0x00012400, 0x00012402, 0x00012410, 0x00012412, 0x00012480, 0x00012482, 0x00012490,
    0x00012492, 0x00080000, 0x00080002, 0x00080010, 0x00080012, 0x00080080, 0x00080082, 0x00080090, 0x00080092,
    0x00080400, 0x00080402, 0x00080410, 0x00080412, 0x00080480, 0x00080482, 0x00080490, 0x00080492, 0x00082000,
    0x00082002, 0x00082010, 0x00082012, 0x00082080, 0x00082082, 0x00082090, 0x00082092, 0x00082400, 0x00082402,
    0x00082410, 0x00082412, 0x00082480, 0x00082482, 0x00082490, 0x00082492, 0x00090000, 0x00090002, 0x00090010,
    0x00090012, 0x00090080, 0x00090082, 0x00090090, 0x00090092, 0x00090400, 0x00090402, 0x00090410, 0x00090412,
    0x00090480, 0x00090482, 0x00090490, 0x00090492, 0x00092000, 0x00092002, 0x00092010, 0x00092012, 0x00092080,
    0x00092082, 0x00092090, 0x00092092, 0x00092400, 0x00092402, 0x00092410, 0x00092412, 0x00092480, 0x00092482,
    0x00092490, 0x00092492, 0x00400000, 0x00400002, 0x00400010, 0x00400012, 0x00400080, 0x00400082, 0x00400090,
    0x00400092, 0x00400400, 0x00400402, 0x00400410, 0x00400412, 0x00400480, 0x00400482, 0x00400490, 0x00400492,
    0x00402000, 0x00402002, 0x00402010, 0x00402012, 0x00402080, 0x00402082, 0x00402090, 0x00402092, 0x00402400,
    0x00402402, 0x00402410, 0x00402412, 0x00402480, 0x00402482, 0x00402490, 0x00402492, 0x00410000, 0x00410002,
    0x00410010, 0x00410012, 0x00410080, 0x00410082, 0x00410090, 0x00410092, 0x00410400, 0x00410402, 0x00410410,
    0x00410412, 0x00410480, 0x00410482, 0x00410490, 0x00410492, 0x00412000, 0x00412002, 0x00412010, 0x00412012,
    0x00412080, 0x00412082, 0x00412090, 0x00412092, 0x00412400, 0x00412402, 0x00412410, 0x00412412, 0x00412480,
    0x00412482, 0x00412490, 0x00412492, 0x00480000, 0x00480002, 0x00480010, 0x00480012, 0x00480080, 0x00480082,
    0x00480090, 0x00480092, 0x00480400, 0x00480402, 0x00480410, 0x00480412, 0x00480480, 0x00480482, 0x00480490,
    0x00480492, 0x00482000, 0x00482002, 0x00482010, 0x00482012, 0x00482080, 0x00482082, 0x00482090, 0x00482092,
    0x00482400, 0x00482402, 0x00482410, 0x00482412, 0x00482480, 0x00482482, 0x00482490, 0x00482492, 0x00490000,
    0x00490002, 0x00490010, 0x00490012, 0x00490080, 0x00490082, 0x00490090, 0x00490092, 0x00490400, 0x00490402,
    0x00490410, 0x00490412, 0x00490480, 0x00490482, 0x00490490, 0x00490492, 0x00492000, 0x00492002, 0x00492010,
    0x00492012, 0x00492080, 0x00492082, 0x00492090, 0x00492092, 0x00492400, 0x00492402, 0x00492410, 0x00492412,
    0x00492480, 0x00492482, 0x00492490, 0x00492492};

const uint32_t kMortonCode256X[256] = {
    0x00000000, 0x00000004, 0x00000020, 0x00000024, 0x00000100, 0x00000104, 0x00000120, 0x00000124, 0x00000800,
    0x00000804, 0x00000820, 0x00000824, 0x00000900, 0x00000904, 0x00000920, 0x00000924, 0x00004000, 0x00004004,
    0x00004020, 0x00004024, 0x00004100, 0x00004104, 0x00004120, 0x00004124, 0x00004800, 0x00004804, 0x00004820,
    0x00004824, 0x00004900, 0x00004904, 0x00004920, 0x00004924, 0x00020000, 0x00020004, 0x00020020, 0x00020024,
    0x00020100, 0x00020104, 0x00020120, 0x00020124, 0x00020800, 0x00020804, 0x00020820, 0x00020824, 0x00020900,
    0x00020904, 0x00020920, 0x00020924, 0x00024000, 0x00024004, 0x00024020, 0x00024024, 0x00024100, 0x00024104,
    0x00024120, 0x00024124, 0x00024800, 0x00024804, 0x00024820, 0x00024824, 0x00024900, 0x00024904, 0x00024920,
    0x00024924, 0x00100000, 0x00100004, 0x00100020, 0x00100024, 0x00100100, 0x00100104, 0x00100120, 0x00100124,
    0x00100800, 0x00100804, 0x00100820, 0x00100824, 0x00100900, 0x00100904, 0x00100920, 0x00100924, 0x00104000,
    0x00104004, 0x00104020, 0x00104024, 0x00104100, 0x00104104, 0x00104120, 0x00104124, 0x00104800, 0x00104804,
    0x00104820, 0x00104824, 0x00104900, 0x00104904, 0x00104920, 0x00104924, 0x00120000, 0x00120004, 0x00120020,
    0x00120024, 0x00120100, 0x00120104, 0x00120120, 0x00120124, 0x00120800, 0x00120804, 0x00120820, 0x00120824,
    0x00120900, 0x00120904, 0x00120920, 0x00120924, 0x00124000, 0x00124004, 0x00124020, 0x00124024, 0x00124100,
    0x00124104, 0x00124120, 0x00124124, 0x00124800, 0x00124804, 0x00124820, 0x00124824, 0x00124900, 0x00124904,
    0x00124920, 0x00124924, 0x00800000, 0x00800004, 0x00800020, 0x00800024, 0x00800100, 0x00800104, 0x00800120,
    0x00800124, 0x00800800, 0x00800804, 0x00800820, 0x00800824, 0x00800900, 0x00800904, 0x00800920, 0x00800924,
    0x00804000, 0x00804004, 0x00804020, 0x00804024, 0x00804100, 0x00804104, 0x00804120, 0x00804124, 0x00804800,
    0x00804804, 0x00804820, 0x00804824, 0x00804900, 0x00804904, 0x00804920, 0x00804924, 0x00820000, 0x00820004,
    0x00820020, 0x00820024, 0x00820100, 0x00820104, 0x00820120, 0x00820124, 0x00820800, 0x00820804, 0x00820820,
    0x00820824, 0x00820900, 0x00820904, 0x00820920, 0x00820924, 0x00824000, 0x00824004, 0x00824020, 0x00824024,
    0x00824100, 0x00824104, 0x00824120, 0x00824124, 0x00824800, 0x00824804, 0x00824820, 0x00824824, 0x00824900,
    0x00824904, 0x00824920, 0x00824924, 0x00900000, 0x00900004, 0x00900020, 0x00900024, 0x00900100, 0x00900104,
    0x00900120, 0x00900124, 0x00900800, 0x00900804, 0x00900820, 0x00900824, 0x00900900, 0x00900904, 0x00900920,
    0x00900924, 0x00904000, 0x00904004, 0x00904020, 0x00904024, 0x00904100, 0x00904104, 0x00904120, 0x00904124,
    0x00904800, 0x00904804, 0x00904820, 0x00904824, 0x00904900, 0x00904904, 0x00904920, 0x00904924, 0x00920000,
    0x00920004, 0x00920020, 0x00920024, 0x00920100, 0x00920104, 0x00920120, 0x00920124, 0x00920800, 0x00920804,
    0x00920820, 0x00920824, 0x00920900, 0x00920904, 0x00920920, 0x00920924, 0x00924000, 0x00924004, 0x00924020,
    0x00924024, 0x00924100, 0x00924104, 0x00924120, 0x00924124, 0x00924800, 0x00924804, 0x00924820, 0x00924824,
    0x00924900, 0x00924904, 0x00924920, 0x00924924};

inline uint64_t PCCEncoder::mortonAddr( const int32_t x, const int32_t y, const int32_t z ) {
  uint64_t answer =
      kMortonCode256X[( x >> 16 ) & 0xFF] | kMortonCode256Y[( y >> 16 ) & 0xFF] | kMortonCode256Z[( z >> 16 ) & 0xFF];
  answer = answer << 24 | kMortonCode256X[( x >> 8 ) & 0xFF] | kMortonCode256Y[( y >> 8 ) & 0xFF] |
           kMortonCode256Z[( z >> 8 ) & 0xFF];
  answer = answer << 24 | kMortonCode256X[x & 0xFF] | kMortonCode256Y[y & 0xFF] | kMortonCode256Z[z & 0xFF];
  return answer;
}
uint64_t PCCEncoder::mortonAddr( const PCCPoint3D& vec, int depth ) {
  int x = int( vec.x() ) >> depth;
  int y = int( vec.y() ) >> depth;
  int z = int( vec.z() ) >> depth;
  return mortonAddr( x, y, z );
}

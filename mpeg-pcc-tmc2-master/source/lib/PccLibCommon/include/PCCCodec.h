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
#ifndef PCCCodec_h
#define PCCCodec_h

#include "tbb/compat/condition_variable"
#include "PCCCommon.h"
#include "PCCImage.h"
#include "PCCMath.h"
#include "PCCVideo.h"
#include "PCCContext.h"

#if OCCUPANCY_MAP_MODEL 
#include <torch/script.h>
#include <torch/csrc/api/include/torch/utils.h>
#include <iostream>
#include <memory>
#endif


namespace pcc {
class PCCPatch;
} /* namespace pcc */

namespace pcc {

class PCCContext;
class PCCFrameContext;
class PCCGroupOfFrames;
class PCCPointSet3;
template <typename T, size_t N>
class PCCVideo;
typedef pcc::PCCVideo<uint8_t, 3>  PCCVideoTexture;
typedef pcc::PCCVideo<uint16_t, 3> PCCVideoGeometry;

template <typename T, size_t N>
class PCCImage;
typedef pcc::PCCImage<uint16_t, 3> PCCImageGeometry;
typedef pcc::PCCImage<uint8_t, 3>  PCCImageOccupancyMap;

struct GeneratePointCloudParameters {
  size_t      occupancyResolution_;
  size_t      occupancyPrecision_;
  bool        enableSizeQuantization_;
  bool        gridSmoothing_;
  size_t      gridSize_;
  size_t      neighborCountSmoothing_;
  double      radius2Smoothing_;
  double      radius2BoundaryDetection_;
  double      thresholdSmoothing_;
  size_t      rawPointColorFormat_;
  size_t      nbThread_;
  bool        multipleStreams_;
  bool        absoluteD1_;
  size_t      surfaceThickness_;
  double      thresholdColorSmoothing_;
  bool        gridColorSmoothing_;
  size_t      cgridSize_;
  double      thresholdColorDifference_;
  double      thresholdColorVariation_;
  double      thresholdLocalEntropy_;
  double      radius2ColorSmoothing_;
  size_t      neighborCountColorSmoothing_;
  bool        flagGeometrySmoothing_;
  bool        flagColorSmoothing_;
  bool        enhancedDeltaDepthCode_;
  size_t      EOMFixBitCount_;
  size_t      thresholdLossyOM_;
  bool        removeDuplicatePoints_;
  size_t      mapCountMinus1_;
  bool        pointLocalReconstruction_;
  bool        singleMapPixelInterleaving_;
  std::string path_;
  bool        useAdditionalPointsPatch_;
  size_t      plrlNumberOfModes_;
  size_t      geometryBitDepth3D_;
  size_t      geometry3dCoordinatesBitdepth_;
  bool        pbfEnableFlag_;
  int16_t     pbfPassesCount_;
  int16_t     pbfFilterSize_;
  int16_t     pbfLog2Threshold_;
};

#ifdef CODEC_TRACE
#define TRACE_CODEC( fmt, ... ) trace( fmt, ##__VA_ARGS__ );
#else
#define TRACE_CODEC( fmt, ... ) ;
#endif

class PCCCodec {
 public:
  PCCCodec();
  ~PCCCodec();

  void generatePointCloud( PCCGroupOfFrames&                   reconstructs,
                           PCCContext&                         context,
                           const GeneratePointCloudParameters  params,
                           std::vector<std::vector<uint32_t>>& partitions,
                           bool                                bDecoder );

  bool colorPointCloud( PCCGroupOfFrames&                     reconstructs,
                        PCCContext&                           context,
                        const uint8_t                         attributeCount,
                        const PCCColorTransform               colorTransform,
                        const std::vector<std::vector<bool>>& absoluteT1List,
                        const size_t                          multipleStreams,
                        const GeneratePointCloudParameters    params );

  void smoothPointCloudPostprocess( PCCGroupOfFrames&                   reconstructs,
                                    PCCContext&                         context,
                                    const PCCColorTransform             colorTransform,
                                    const GeneratePointCloudParameters  params,
                                    std::vector<std::vector<uint32_t>>& partitions );

  void colorSmoothing( PCCGroupOfFrames&                  reconstructs,
                       PCCContext&                        context,
                       const PCCColorTransform            colorTransform,
                       const GeneratePointCloudParameters params );

  void generateMPsGeometryfromImage( PCCContext&       context,
                                     PCCFrameContext&  frame,
                                     PCCGroupOfFrames& reconstructs,
                                     size_t            frameIndex );

  void generateMPsTexturefromImage( PCCContext&       context,
                                    PCCFrameContext&  frame,
                                    PCCGroupOfFrames& reconstructs,
                                    size_t            frameIndex );

  void generateMissedPointsGeometryfromVideo( PCCContext& context, PCCGroupOfFrames& reconstructs );

  void generateMissedPointsTexturefromVideo( PCCContext& context, PCCGroupOfFrames& reconstructs );

  void generateOccupancyMap( PCCContext&  context,
                             const size_t occupancyPrecision,
                             const size_t thresholdLossyOM,
                             bool         enhancedOccupancyMapForDepthFlag );

#if OCCUPANCY_MAP_MODEL
  void upsampleOccupancyMap( PCCContext& context, PCCVideoOccupancyMap&  om);
  void processIngredient(std::string model_name,  PCCContext& context, PCCVideoOccupancyMap&  om);
  void refineOccupancy( PCCVideoOccupancyMap vom_org,  PCCVideoOccupancyMap& videoOccupancyMap);
#endif
 
#ifdef CODEC_TRACE
  template <typename... Args>
  void trace( const char* pFormat, Args... eArgs ) {
    if ( trace_ ) {
      FILE* output = traceFile_ ? traceFile_ : stdout;
      fprintf( output, pFormat, eArgs... );
      fflush( output );
    }
  }

  template <typename T>
  void traceVector( std::vector<T>    data,
                    const size_t      width,
                    const size_t      height,
                    const std::string string,
                    const bool        hexa = false ) {
    if ( trace_ ) {
      if ( data.size() == 0 ) { data.resize( width * height, 0 ); }
      trace( "%s: %lu %lu \n", string.c_str(), width, height );
      for ( size_t v0 = 0; v0 < height; ++v0 ) {
        for ( size_t u0 = 0; u0 < width; ++u0 ) {
          if ( hexa ) {
            trace( "%2x", (int)( data[v0 * width + u0] ) );
          } else {
            trace( "%3d", (int)( data[v0 * width + u0] ) );
          }
        }
        trace( "\n" );
      }
    }
  }

  void setTrace( bool trace ) { trace_ = trace; }
  bool getTrace() { return trace_; }
  bool openTrace( std::string file ) {
    if ( traceFile_ ) {
      fclose( traceFile_ );
      traceFile_ = NULL;
    }
    if ( ( traceFile_ = fopen( file.c_str(), "w" ) ) == NULL ) { return false; }
    return true;
  }
  void closeTrace() {
    if ( traceFile_ ) {
      fclose( traceFile_ );
      traceFile_ = NULL;
    }
  }
#endif
 protected:
  void generateOccupancyMap( PCCFrameContext&            frame,
                             const PCCImageOccupancyMap& videoFrame,
                             const size_t                occupancyPrecision,
                             const size_t                thresholdLossyOM,
                             const bool                  enhancedOccupancyMapForDepthFlag );

  void generateBlockToPatchFromOccupancyMap( PCCContext& context, const size_t occupancyResolution, bool bDecoder );

  void generateBlockToPatchFromOccupancyMap( PCCContext&      context,
                                             PCCFrameContext& frame,
                                             size_t           frameIndex,
                                             const size_t     occupancyResolution,
                                             bool             bDecoder );

  void generateBlockToPatchFromBoundaryBox( PCCContext& context, const size_t occupancyResolution );

  void generateBlockToPatchFromBoundaryBox( PCCContext&      context,
                                            PCCFrameContext& frame,
                                            size_t           frameIndex,
                                            const size_t     occupancyResolution );

  void generateBlockToPatchFromOccupancyMapVideo( PCCContext&  context,
                                                  const bool   losslessGeo,
                                                  const bool   lossyMissedPointsPatch,
                                                  const size_t occupancyResolution,
                                                  const size_t occupancyPrecision );

  void generateBlockToPatchFromOccupancyMapVideo( PCCContext&           context,
                                                  PCCFrameContext&      frame,
                                                  PCCImageOccupancyMap& occupancyMapImage,
                                                  size_t                frameIndex,
                                                  const size_t          occupancyResolution,
                                                  const size_t          occupancyPrecision );

  int getDeltaNeighbors( const PCCImageGeometry& frame,
                         const PCCPatch&         patch,
                         const int               xOrg,
                         const int               yOrg,
                         const int               neighboring,
                         const int               threshold,
                         const bool              projectionMode );

  std::vector<PCCPoint3D> generatePoints( const GeneratePointCloudParameters& params,
                                          PCCFrameContext&                    frame,
                                          const PCCVideoGeometry&             video,
                                          const PCCVideoGeometry&             videoD1,
                                          const size_t                        shift,
                                          const size_t                        patchIndex,
                                          const size_t                        u,
                                          const size_t                        v,
                                          const size_t                        x,
                                          const size_t                        y,
                                          const bool                          interpolate,
                                          const bool                          filling,
                                          const size_t                        minD1,
                                          const size_t                        neighbor );
  PCCPatchType            getCurrPatchType( PCCTILEGROUP tileGroupType, uint8_t patchMode );
  inline double           entropy( std::vector<uint8_t>& Data, int N ) {
    std::vector<size_t> count;
    count.resize( 256, 0 );
    for ( size_t i = 0; i < N; ++i ) { ++count[size_t( Data[i] )]; }
    double s = 0;
    for ( size_t i = 0; i < 256; ++i ) {
      if ( count[i] ) {
        double p = double( count[i] ) / double( N );
        s += -p * std::log2( p );
      }
    }
    return s;
  }

  inline double median( std::vector<uint8_t>& Data, int N ) {
    float med    = 0;
    int   a      = 0;
    int   b      = 0;
    float newMed = 0;
    if ( N % 2 == 0 ) a = N / 2;
    b      = ( N / 2 ) - 1;
    med    = int( Data.at( a ) ) + Data.at( b );
    newMed = ( med / 2 );
    return double( newMed );
  }

  inline double mean( std::vector<uint8_t>& Data, int N ) {
    double s = 0.0;
    for ( size_t i = 0; i < N; ++i ) { s += double( Data[i] ); }
    return s / double( N );
  }

 private:
  void generatePointCloud( PCCPointSet3&                      reconstruct,
                           PCCContext&                        context,
                           PCCFrameContext&                   frame,
                           const PCCVideoGeometry&            video,
                           const PCCVideoGeometry&            videoD1,
                           const PCCVideoOccupancyMap&        videoOM,
                           const GeneratePointCloudParameters params,
                           std::vector<uint32_t>&             partition,
                           bool                               bDecoder );

  void smoothPointCloud( PCCPointSet3&                      reconstruct,
                         const std::vector<uint32_t>&       partition,
                         const GeneratePointCloudParameters params );

  void createSubReconstruct( const PCCPointSet3&                 reconstruct,
                             const std::vector<uint32_t>&        partition,
                             PCCFrameContext&                    frame,
                             const GeneratePointCloudParameters& params,
                             const size_t                        frameCount,
                             PCCPointSet3&                       subReconstruct,
                             std::vector<uint32_t>&              subPartition,
                             std::vector<size_t>&                subReconstructIndex );

  void createSpecificLayerReconstruct( const PCCPointSet3&                 reconstruct,
                                       const std::vector<uint32_t>&        partition,
                                       PCCFrameContext&                    frame,
                                       const GeneratePointCloudParameters& params,
                                       const size_t                        frameCount,
                                       PCCPointSet3&                       subReconstruct,
                                       std::vector<uint32_t>&              subPartition,
                                       std::vector<size_t>&                subReconstructIndex );

  void updateReconstruct( PCCPointSet3&              reconstruct,
                          const PCCPointSet3&        subReconstruct,
                          const std::vector<size_t>& subReconstructIndex );

  bool colorPointCloud( PCCPointSet3&                       reconstruct,
                        PCCContext&                         context,
                        size_t                              frameIndex,
                        const std::vector<bool>&            absoluteT1List,
                        const size_t                        multipleStreams,
                        const uint8_t                       attributeCount,
                        const GeneratePointCloudParameters& params );

  void smoothPointCloudColor( PCCPointSet3& reconstruct, const GeneratePointCloudParameters params );

  void smoothPointCloudGrid( PCCPointSet3&                      reconstruct,
                             const std::vector<uint32_t>&       partition,
                             const GeneratePointCloudParameters params,
                             int                                gridWidth );

  void addGridCentroid( PCCPoint3D&               point,
                        int                       patchIdx,
                        std::vector<int>&         count,
                        std::vector<PCCVector3D>& center,
                        std::vector<int>&         partition,
                        std::vector<bool>&        doSmooth,
                        int                       gridSize,
                        int                       gridWidth );

  void addGridColorCentroid( PCCPoint3D&                        point,
                             PCCVector3D&                       color,
                             int                                patchIdx,
                             std::vector<int>&                  colorCount,
                             std::vector<PCCVector3D>&          colorCenter,
                             std::vector<int>&                  colorPartition,
                             std::vector<bool>&                 colorDoSmooth,
                             int                                colorgrid,
                             std::vector<std::vector<uint8_t>>& colorLum,
                             const GeneratePointCloudParameters params );

  bool gridFilteringColor( PCCPoint3D&                        curPos,
                           PCCVector3D&                       colorCentroid,
                           int&                               colorCount,
                           std::vector<int>&                  colorGridCount,
                           std::vector<PCCVector3D>&          colorCenterGrid,
                           std::vector<bool>&                 colorDoSmooth,
                           int                                colorGrid,
                           PCCVector3D&                       curPosColor,
                           const GeneratePointCloudParameters params );

  void smoothPointCloudColorLC( PCCPointSet3& reconstruct, const GeneratePointCloudParameters params );

  bool gridFiltering( const std::vector<uint32_t>& partition,
                      PCCPointSet3&                pointCloud,
                      PCCPoint3D&                  curPos,
                      PCCVector3D&                 centroid,
                      int&                         count,
                      std::vector<int>&            gridCount,
                      std::vector<PCCVector3D>&    center,
                      std::vector<bool>&           doSmooth,
                      int                          gridSize,
                      int                          gridWidth );

  bool gridFilteringTransfer( const std::vector<uint32_t>& partition,
                              PCCPointSet3&                pointCloud,
                              PCCPoint3D&                  curPos,
                              PCCVector3D&                 centroid,
                              int&                         count,
                              std::vector<int>&            gridCount,
                              std::vector<PCCVector3D>&    center,
                              std::vector<bool>&           doSmooth,
                              int                          gridSize,
                              int                          gridWidth,
                              std::vector<PCCVector3D>&    colorGrid,
                              PCCVector3D&                 color );

  void identifyBoundaryPoints( const std::vector<uint32_t>& occupancyMap,
                               const size_t                 x,
                               const size_t                 y,
                               const size_t                 imageWidth,
                               const size_t                 imageHeight,
                               const size_t                 pointIndex,
                               std::vector<uint32_t>&       PBflag,
                               PCCPointSet3&                reconstruct );

  std::vector<int>                  geoSmoothingCount_;
  std::vector<PCCVector3D>          geoSmoothingCenter_;
  std::vector<bool>                 geoSmoothingDoSmooth_;
  std::vector<int>                  geoSmoothingPartition_;
  std::vector<int>                  colorSmoothingCount_;
  std::vector<PCCVector3D>          colorSmoothingCenter_;
  std::vector<bool>                 colorSmoothingDoSmooth_;
  std::vector<int>                  colorSmoothingPartition_;
  std::vector<std::vector<uint8_t>> colorSmoothingLum_;
#ifdef CODEC_TRACE
  bool  trace_;
  FILE* traceFile_;
#else
#ifdef BITSTREAM_TRACE
  bool  trace_;
  FILE* traceFile_;
#endif
#endif
};

};  // namespace pcc

#endif /* PCCCodec_h */

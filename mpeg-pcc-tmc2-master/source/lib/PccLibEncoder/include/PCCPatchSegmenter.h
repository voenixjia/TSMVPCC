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

#ifndef PCCPatchSegmenter_h
#define PCCPatchSegmenter_h

#include "PCCCommon.h"
#include <set>

namespace pcc {

class PCCNormalsGenerator3;
class PCCKdTree;
class PCCPatch;

struct PCCPatchSegmenter3Parameters {
  size_t           nnNormalEstimation_;
  bool             gridBasedRefineSegmentation_;
  size_t           maxNNCountRefineSegmentation_;
  size_t           iterationCountRefineSegmentation_;
  size_t           voxelDimensionRefineSegmentation_;
  size_t           searchRadiusRefineSegmentation_;
  size_t           occupancyResolution_;
  size_t           quantizerSizeX_;
  size_t           quantizerSizeY_;
  size_t           minPointCountPerCCPatchSegmentation_;
  size_t           maxNNCountPatchSegmentation_;
  size_t           surfaceThickness_;
  size_t           EOMFixBitCount_;
  bool             EOMSingleLayerMode_;
  size_t           minLevel_;
  size_t           maxAllowedDepth_;
  double           maxAllowedDist2MissedPointsDetection_;
  double           maxAllowedDist2MissedPointsSelection_;
  double           lambdaRefineSegmentation_;
  bool             useEnhancedDeltaDepthCode_;
  bool             absoluteD1_;
  bool             createSubPointCloud_;
  bool             surfaceSeparation_;
  PCCVector3D      weightNormal_;
  size_t           additionalProjectionPlaneMode_;
  double           partialAdditionalProjectionPlane_;
  size_t           geometryBitDepth3D_;
  bool             patchExpansion_;
  bool             highGradientSeparation_;
  double           minGradient_;
  size_t           minNumHighGradientPoints_;
  bool             enablePointCloudPartitioning_;
  std::vector<int> roiBoundingBoxMinX_;
  std::vector<int> roiBoundingBoxMaxX_;
  std::vector<int> roiBoundingBoxMinY_;
  std::vector<int> roiBoundingBoxMaxY_;
  std::vector<int> roiBoundingBoxMinZ_;
  std::vector<int> roiBoundingBoxMaxZ_;
  int              numTilesHor_;
  double           tileHeightToWidthRatio_;
  int              numCutsAlong1stLongestAxis_;
  int              numCutsAlong2ndLongestAxis_;
  int              numCutsAlong3rdLongestAxis_;
};

class PCCPatchSegmenter3 {
 public:
  PCCPatchSegmenter3( void ) : nbThread_( 0 ) {}
  PCCPatchSegmenter3( const PCCPatchSegmenter3& ) = delete;
  PCCPatchSegmenter3& operator=( const PCCPatchSegmenter3& ) = delete;
  ~PCCPatchSegmenter3()                                      = default;
  void setNbThread( size_t nbThread );

  void compute( const PCCPointSet3&                 geometry,
                const size_t                        frameIndex,
                const PCCPatchSegmenter3Parameters& params,
                std::vector<PCCPatch>&              patches,
                std::vector<PCCPointSet3>&          subPointCloud,
                float&                              distanceSrcRec );

  void initialSegmentation( const PCCPointSet3&         geometry,
                            const PCCNormalsGenerator3& normalsGen,
                            const PCCVector3D*          orientations,
                            const size_t                orientationCount,
                            std::vector<size_t>&        partition,
                            const PCCVector3D           axis_weight );
  void initialSegmentation( const PCCPointSet3&         geometry,
                            const PCCNormalsGenerator3& normalsGen,
                            const PCCVector3D*          orientations,
                            const size_t                orientationCount,
                            std::vector<size_t>&        partition );
  void computeAdjacencyInfo( const PCCPointSet3&               pointCloud,
                             const PCCKdTree&                  kdtree,
                             std::vector<std::vector<size_t>>& adj,
                             const size_t                      maxNNCount );

  void computeAdjacencyInfoDist( const PCCPointSet3&               pointCloud,
                                 const PCCKdTree&                  kdtree,
                                 std::vector<std::vector<size_t>>& adj,
                                 std::vector<std::vector<double>>& adjDist,
                                 const size_t                      maxNNCount );

  void computeAdjacencyInfoInRadius( const PCCPointSet3&               pointCloud,
                                     const PCCKdTree&                  kdtree,
                                     std::vector<std::vector<size_t>>& adj,
                                     const size_t                      maxNNCount,
                                     const size_t                      radius );

  void segmentPatches( const PCCPointSet3&                 points,
                       const size_t                        frameIndex,
                       const PCCKdTree&                    kdtree,
                       const PCCPatchSegmenter3Parameters& params,
                       std::vector<size_t>&                partition,
                       std::vector<PCCPatch>&              patches,
                       std::vector<size_t>&                patchPartition,
                       std::vector<size_t>&                resampledPatchPartition,
                       std::vector<size_t>                 missedPoints,
                       PCCPointSet3&                       resampled,
                       std::vector<PCCPointSet3>&          subPointCloud,
                       float&                              distanceSrcRec,
                       const PCCNormalsGenerator3&         normalsGen,
                       const PCCVector3D*                  orientations,
                       const size_t                        orientationCount );

  void refineSegmentation( const PCCPointSet3&         pointCloud,
                           const PCCKdTree&            kdtree,
                           const PCCNormalsGenerator3& normalsGen,
                           const PCCVector3D*          orientations,
                           const size_t                orientationCount,
                           const size_t                maxNNCount,
                           const double                lambda,
                           const size_t                iterationCount,
                           std::vector<size_t>&        partition );

  void refineSegmentationGridBased( const PCCPointSet3&         pointCloud,
                                    const PCCNormalsGenerator3& normalsGen,
                                    const PCCVector3D*          orientations,
                                    const size_t                orientationCount,
                                    const size_t                maxNNCount,
                                    const double                lambda,
                                    const size_t                iterationCount,
                                    const size_t                voxelDimensionRefineSegmentation,
                                    const size_t                searchRadiusRefineSegmentation,
                                    std::vector<size_t>&        partition );

 private:
  size_t                nbThread_;
  std::vector<PCCPatch> boxMinDepths_;  // box depth list
  std::vector<PCCPatch> boxMaxDepths_;  // box depth list

  void convert( size_t Axis, size_t lod, PCCPoint3D input, PCCPoint3D& output ) {
    size_t shif = ( 1u << lod ) - 1;
    output      = input;
    if ( Axis == 1 ) {  // Additional plane are defined by Y Axis.
      output.x() = input.x() + input.z();
      output.z() = -input.x() + input.z() + shif;
    }
    if ( Axis == 2 ) {  // Additional plane are defined by X Axis.
      output.z() = input.z() + input.y();
      output.y() = -input.z() + input.y() + shif;
    }
    if ( Axis == 3 ) {
      output.y() = input.y() + input.x();
      output.x() = -input.y() + input.x() + shif;
    }
  }

  void iconvert( size_t Axis, size_t lod, PCCVector3D input, PCCVector3D& output ) {
    size_t shif = ( 1u << lod ) - 1;
    // output = input;
    output.x() = input.x();
    output.y() = input.y();
    output.z() = input.z();

    if ( Axis == 1 ) {  // Additional plane are defined by Y Axis.
      output.x() = input.x() - input.z() + shif;
      output.x() /= 2.0;

      output.z() = input.x() + input.z() - shif;
      output.z() /= 2.0;
    }
    if ( Axis == 2 ) {
      output.z() = input.z() - input.y() + shif;
      output.z() /= 2.0;

      output.y() = input.z() + input.y() - shif;
      output.y() /= 2.0;
    }  // not implemented yet
    if ( Axis == 3 ) {
      output.y() = input.y() - input.x() + shif;
      output.y() /= 2.0;

      output.x() = input.y() + input.x() - shif;
      output.x() /= 2.0;
    }
  }

  void separateHighGradientPoints( const PCCPointSet3&               points,
                                   const size_t                      additionalProjectionAxis,
                                   const bool                        absoluteD1,
                                   const PCCNormalsGenerator3&       normalsGen,
                                   const PCCVector3D*                orientations,
                                   const size_t                      orientationCount,
                                   const size_t                      surfaceThickness,
                                   const size_t                      geometryBitDepth3D,
                                   const double                      minGradient,
                                   const size_t                      minNumHighGradientPoints,
                                   std::vector<size_t>&              partition,
                                   std::vector<std::vector<size_t>>& adj,
                                   std::vector<std::vector<size_t>>& connectedComponents );
  void determinePatchOrientation( const size_t         additionalProjectionAxis,
                                  const bool           absoluteD1,
                                  bool&                bIsAdditionalProjectionPlane,
                                  PCCPatch&            patch,
                                  std::vector<size_t>& partition,
                                  std::vector<size_t>& connectedComponent );
  void generatePatchD0( const PCCPointSet3&  points,
                        const size_t         geometryBitDepth3D,
                        const bool           bIsAdditionalProjectionPlane,
                        PCCPatch&            patch,
                        std::vector<size_t>& connectedComponent );
  void calculateGradient( const PCCPointSet3&               points,
                          const std::vector<size_t>&        connectedComponent,
                          const PCCNormalsGenerator3&       normalsGen,
                          const PCCVector3D*                orientations,
                          const size_t                      orientationCount,
                          const size_t                      orgPartitionIdx,
                          const size_t                      surfaceThickness,
                          const size_t                      geometryBitDepth3D,
                          const bool                        bIsAdditionalProjectionPlane,
                          const double                      minGradient,
                          const size_t                      minNumHighGradientPoints,
                          PCCPatch&                         patch,
                          std::vector<std::vector<size_t>>& adj,
                          std::vector<std::vector<size_t>>& highGradientConnectedComponents,
                          std::vector<bool>&                isRemoved );

  PCCVector3D orientations6[6] = {
      PCCVector3D( 1.0, 0.0, 0.0 ),  PCCVector3D( 0.0, 1.0, 0.0 ),  PCCVector3D( 0.0, 0.0, 1.0 ),
      PCCVector3D( -1.0, 0.0, 0.0 ), PCCVector3D( 0.0, -1.0, 0.0 ), PCCVector3D( 0.0, 0.0, -1.0 ),
  };
  const size_t orientationCount6 = 6;
  //  const size_t orientation10Count = 10;
  PCCVector3D orientations10_XAxis[10] = {
      PCCVector3D( 1.0, 0.0, 0.0 ),                        // 0
      PCCVector3D( 0.0, 1.0, 0.0 ),                        // 1
      PCCVector3D( 0.0, 0.0, 1.0 ),                        // 2
      PCCVector3D( -1.0, 0.0, 0.0 ),                       // 3
      PCCVector3D( 0.0, -1.0, 0.0 ),                       // 4
      PCCVector3D( 0.0, 0.0, -1.0 ),                       // 5
      PCCVector3D( 0.0, sqrt( 2 ) / 2, sqrt( 2 ) / 2 ),    // 6
      PCCVector3D( 0.0, -sqrt( 2 ) / 2, sqrt( 2 ) / 2 ),   // 7
      PCCVector3D( 0.0, -sqrt( 2 ) / 2, -sqrt( 2 ) / 2 ),  // 8
      PCCVector3D( 0.0, sqrt( 2 ) / 2, -sqrt( 2 ) / 2 ),   // 9
  };

  PCCVector3D orientations10_YAxis[10] = {
      PCCVector3D( 1.0, 0.0, 0.0 ),                        // 0
      PCCVector3D( 0.0, 1.0, 0.0 ),                        // 1
      PCCVector3D( 0.0, 0.0, 1.0 ),                        // 2
      PCCVector3D( -1.0, 0.0, 0.0 ),                       // 3
      PCCVector3D( 0.0, -1.0, 0.0 ),                       // 4
      PCCVector3D( 0.0, 0.0, -1.0 ),                       // 5
      PCCVector3D( sqrt( 2 ) / 2, 0.0, sqrt( 2 ) / 2 ),    // 6
      PCCVector3D( -sqrt( 2 ) / 2, 0.0, sqrt( 2 ) / 2 ),   // 7
      PCCVector3D( -sqrt( 2 ) / 2, 0.0, -sqrt( 2 ) / 2 ),  // 8
      PCCVector3D( sqrt( 2 ) / 2, 0.0, -sqrt( 2 ) / 2 ),   // 9
  };

  PCCVector3D orientations10_ZAxis[10] = {
      PCCVector3D( 1.0, 0.0, 0.0 ),                        // 0
      PCCVector3D( 0.0, 1.0, 0.0 ),                        // 1
      PCCVector3D( 0.0, 0.0, 1.0 ),                        // 2
      PCCVector3D( -1.0, 0.0, 0.0 ),                       // 3
      PCCVector3D( 0.0, -1.0, 0.0 ),                       // 4
      PCCVector3D( 0.0, 0.0, -1.0 ),                       // 5
      PCCVector3D( sqrt( 2 ) / 2, sqrt( 2 ) / 2, 0.0 ),    // 6
      PCCVector3D( -sqrt( 2 ) / 2, sqrt( 2 ) / 2, 0.0 ),   // 7
      PCCVector3D( -sqrt( 2 ) / 2, -sqrt( 2 ) / 2, 0.0 ),  // 8
      PCCVector3D( sqrt( 2 ) / 2, -sqrt( 2 ) / 2, 0.0 ),   // 9
  };

  //  const size_t orientation18Count = 18;
  PCCVector3D orientations18[18] = {
      PCCVector3D( 1.0, 0.0, 0.0 ),                        // 0
      PCCVector3D( 0.0, 1.0, 0.0 ),                        // 1
      PCCVector3D( 0.0, 0.0, 1.0 ),                        // 2
      PCCVector3D( -1.0, 0.0, 0.0 ),                       // 3
      PCCVector3D( 0.0, -1.0, 0.0 ),                       // 4
      PCCVector3D( 0.0, 0.0, -1.0 ),                       // 5
      PCCVector3D( sqrt( 2 ) / 2, 0.0, sqrt( 2 ) / 2 ),    // 6   1
      PCCVector3D( -sqrt( 2 ) / 2, 0.0, sqrt( 2 ) / 2 ),   // 7
      PCCVector3D( -sqrt( 2 ) / 2, 0.0, -sqrt( 2 ) / 2 ),  // 8
      PCCVector3D( sqrt( 2 ) / 2, 0.0, -sqrt( 2 ) / 2 ),   // 9
      PCCVector3D( 0.0, sqrt( 2 ) / 2, sqrt( 2 ) / 2 ),    // 10   2
      PCCVector3D( 0.0, -sqrt( 2 ) / 2, sqrt( 2 ) / 2 ),   // 11
      PCCVector3D( 0.0, -sqrt( 2 ) / 2, -sqrt( 2 ) / 2 ),  // 12
      PCCVector3D( 0.0, sqrt( 2 ) / 2, -sqrt( 2 ) / 2 ),   // 13
      PCCVector3D( sqrt( 2 ) / 2, sqrt( 2 ) / 2, 0.0 ),    // 14    3
      PCCVector3D( -sqrt( 2 ) / 2, sqrt( 2 ) / 2, 0.0 ),   // 15
      PCCVector3D( -sqrt( 2 ) / 2, -sqrt( 2 ) / 2, 0.0 ),  // 16
      PCCVector3D( sqrt( 2 ) / 2, -sqrt( 2 ) / 2, 0.0 ),   // 17
  };
};

class Rect {
 public:
  Rect& operator=( const Rect& ) = delete;
  ~Rect()                        = default;

  Rect() { x_ = y_ = width_ = height_ = 0; }
  Rect( int x, int y, int w, int h ) {
    this->x_ = x;
    this->y_ = y;
    width_   = w;
    height_  = h;
  }
  int area() { return ( width_ * height_ ); }

  Rect operator&( const Rect& rhs ) {
    Rect c;
    int  x1 = ( this->x_ > rhs.x_ ) ? this->x_ : rhs.x_;
    int  y1 = ( this->y_ > rhs.y_ ) ? this->y_ : rhs.y_;
    c.width_ =
        ( ( ( this->x_ + this->width_ ) < ( rhs.x_ + rhs.width_ ) ) ? this->x_ + this->width_ : rhs.x_ + rhs.width_ ) -
        x1;
    c.height_ = ( ( ( this->y_ + this->height_ ) < ( rhs.y_ + rhs.height_ ) ) ? this->y_ + this->height_
                                                                              : rhs.y_ + rhs.height_ ) -
                y1;
    c.x_ = x1;
    c.y_ = y1;
    if ( c.width_ <= 0 || c.height_ <= 0 ) { c.x_ = c.y_ = c.width_ = c.height_ = 0; }
    return Rect( c );
  }

 private:
  int width_;
  int height_;
  int x_;
  int y_;
};

float computeIOU( Rect a, Rect b );

}  // namespace pcc

#endif /* PCCPatchSegmenter_h */

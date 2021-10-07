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
#ifndef PCCFrameContext_h
#define PCCFrameContext_h

#include "PCCCommon.h"

#include "PCCPointSet.h"
#include "PCCPatch.h"
#include "PCCContext.h"

namespace pcc {
struct PCCGPAFrameSize {
  size_t widthGPA_;
  size_t heightGPA_;
};

struct PCCFrameOCPInfo {
  size_t            occupancySizeU_;
  size_t            occupancySizeV_;
  size_t            maxOccupancyRow_;
  std::vector<bool> occupancyMap_;
};

class PCCFrameContext {
 public:
  PCCFrameContext();
  ~PCCFrameContext();
  std::vector<PCCVector3<size_t>>&   getPointToPixel() { return pointToPixel_; }
  std::vector<size_t>&               getBlockToPatch() { return blockToPatch_; }
  std::vector<uint32_t>&             getOccupancyMap() { return occupancyMap_; }
  std::vector<uint32_t>&             getFullOccupancyMap() { return fullOccupancyMap_; }
  std::vector<PCCPatch>&             getPatches() { return patches_; }
  PCCPatch&                          getPatch( size_t index ) { return patches_[index]; }
  const PCCPatch&                    getPatch( size_t index ) const { return patches_[index]; }
  std::vector<PCCMissedPointsPatch>& getMissedPointsPatches() { return missedPointsPatches_; }
  PCCMissedPointsPatch&              getMissedPointsPatch( size_t index ) { return missedPointsPatches_[index]; }
  std::vector<size_t>&               getNumberOfMissedPoints() { return numberOfMissedPoints_; };
  std::vector<PCCColor3B>&           getMpsTextures() { return mpsTextures_; };
  std::vector<PCCColor3B>&           getEddTextures() { return eddTextures_; };
  size_t&                            getWidth() { return width_; }
  size_t&                            getHeight() { return height_; }
  const size_t                       getIndex() { return index_; }
  const size_t                       getTotalNumberOfEddPoints() { return totalNumberOfEddPoints_; }
  const size_t                       getTotalNumberOfRegularPoints() { return totalNumberOfRegularPoints_; }
  const size_t                       getNumberOfMissedPoints( int index ) { return numberOfMissedPoints_[index]; }
  const size_t                       getNumMatchedPatches() { return numMatchedPatches_; }
  const size_t                       getNumberOfMissedPointsPatches() { return missedPointsPatches_.size(); }
  const size_t                       getTotalNumberOfMissedPoints() { return totalNumberOfMissedPoints_; }
  const size_t                       getMPGeoWidth() { return MPGeoWidth_; }
  const size_t                       getMPGeoHeight() { return MPGeoHeight_; }
  const size_t                       getMPAttWidth() { return MPAttWidth_; }
  const size_t                       getMPAttHeight() { return MPAttHeight_; }
  const bool                         getLosslessGeo() { return losslessGeo_; }
  const bool                         getLosslessGeo444() { return losslessGeo444_; }
  bool                               getUseMissedPointsSeparateVideo() { return useMissedPointsSeparateVideo_; }
  const bool                         getRawPatchEnabledFlag() { return rawPatchEnabledFlag_; }
  const size_t                       getMaxDepth() { return maxDepth_; }
  void                               setMaxDepth( size_t value ) { maxDepth_ = value; }
  size_t                             getGeometry2dNorminalBitdepth() { return geometry2dNorminalBitdepth_; }
  std::vector<PCCPointSet3>&         getSrcPointCloudByPatch() { return srcPointCloudByPatch_; }
  PCCPointSet3&              getSrcPointCloudByPatch( size_t patchIndex ) { return srcPointCloudByPatch_[patchIndex]; }
  std::vector<PCCPointSet3>& getSrcPointCloudByBlock() { return srcPointCloudByBlock_; }
  PCCVector3D&               getWeightNormal() { return weightNormal_; }
  uint8_t&                   getPointLocalReconstructionNumber() { return pointLocalReconstructionNumber_; }
  PCCGPAFrameSize&           getPrePCCGPAFrameSize() { return prePCCGPAFrameSize_; }
  PCCGPAFrameSize&           getCurPCCGPAFrameSize() { return curPCCGPAFrameSize_; }
  PCCFrameOCPInfo&           getPCCOCPGPAInfo() { return ocpGPAInfo_; }
  size_t&                    getGlobalPatchCount() { return globalPatchCount_; }
  size_t                     getGeometry3dCoordinatesBitdepth() { return geometry3dCoordinatesBitdepth_; }
  void                       setIndex( size_t value ) { index_ = value; }
  void                       setWidth( size_t value ) { width_ = value; }
  void                       setHeight( size_t value ) { height_ = value; }
  void                       setMPGeoWidth( size_t width ) { MPGeoWidth_ = width; }
  void                       setMPGeoHeight( size_t height ) { MPGeoHeight_ = height; }
  void                       setMPAttWidth( size_t width ) { MPAttWidth_ = width; }
  void                       setMPAttHeight( size_t height ) { MPAttHeight_ = height; }
  void                       setLosslessGeo( bool lossless ) { losslessGeo_ = lossless; }
  void                       setLosslessGeo444( bool lossless ) { losslessGeo444_ = lossless; }
  void                       setUseMissedPointsSeparateVideo( bool value ) { useMissedPointsSeparateVideo_ = value; }
  void                       setTotalNumberOfEddPoints( size_t numPoints ) { totalNumberOfEddPoints_ = numPoints; }
  void setTotalNumberOfRegularPoints( size_t numPoints ) { totalNumberOfRegularPoints_ = numPoints; }
  void setNumberOfMissedPoints( int index, size_t value ) { numberOfMissedPoints_[index] = value; }
  void setNumberOfMissedPointsPatches( size_t numPoints ) { numberOfMissedPointsPatches_ = numPoints; }
  void setTotalNumberOfMissedPoints( size_t numPoints ) { totalNumberOfMissedPoints_ = numPoints; }
  void setGeometry3dCoordinatesBitdepth( size_t value ) { geometry3dCoordinatesBitdepth_ = value; }
  void setRawPatchEnabledFlag( bool value ) { rawPatchEnabledFlag_ = value; }
  void setGeometry2dNorminalBitdepth( size_t value ) { geometry2dNorminalBitdepth_ = value; }
  void setNumMatchedPatches( size_t value ) { numMatchedPatches_ = value; }

  void                      setEomPatches( PCCEomPatch value, size_t idx ) { eomPatches_[idx] = value; }
  std::vector<PCCEomPatch>& getEomPatches() { return eomPatches_; }
  PCCEomPatch&              getEomPatches( size_t idx ) { return eomPatches_[idx]; }

  void    set2dPosXBitCountMinus1( size_t value ) { bitCount2dPosXMinus1_ = value; }
  void    set2dPosYBitCountMinus1( size_t value ) { bitCount2dPosYMinus1_ = value; }
  void    set3dPosXBitCountMinus1( size_t value ) { bitCount3dPosXMinus1_ = value; }
  void    set3dPosYBitCountMinus1( size_t value ) { bitCount3dPosYMinus1_ = value; }
  size_t  get2dPosXBitCountMinus1() { return bitCount2dPosXMinus1_; }
  size_t  get2dPosYBitCountMinus1() { return bitCount2dPosYMinus1_; }
  size_t  get3dPosXBitCountMinus1() { return bitCount3dPosXMinus1_; }
  size_t  get3dPosYBitCountMinus1() { return bitCount3dPosYMinus1_; }
  uint8_t getLog2PatchQuantizerSizeX() { return log2PatchQuantizerSizeX_; }
  uint8_t getLog2PatchQuantizerSizeY() { return log2PatchQuantizerSizeY_; }
  void    setLog2PatchQuantizerSizeX( uint8_t value ) { log2PatchQuantizerSizeX_ = value; }
  void    setLog2PatchQuantizerSizeY( uint8_t value ) { log2PatchQuantizerSizeY_ = value; }

  void setNumOfRefAFOC( size_t value ) { refAFOCList_[0].resize( value ); }
  void setRefAFOC( size_t refIndex, size_t value ) { refAFOCList_[0][refIndex] = value; }
  void setNumOfRefAtlasFrame( size_t idx, size_t value ) { refAFOCList_[idx].resize( value ); }
  void setRefAFOC( size_t idx, size_t refIndex, size_t value ) { refAFOCList_[idx][refIndex] = value; }
  void setAtlasFrmOrderCntVal( size_t value ) { atlasFrmOrderCntVal_ = value; }
  void setAtlasFrmOrderCntMsb( size_t value ) { atlasFrmOrderCntMsb_ = value; }
  void setAtlasFrmOrderCntLsb( size_t value ) { atlasFrmOrderCntLsb_ = value; }
  void setNumOfRefAtlasFrameList( size_t value ) { numOfAvailableRefAtlasFrameList_ = value; }
  void setActiveRefAtlasFrameIndex( size_t value ) { activeRefAtlasFrameIndex_ = value; }

  void setAFOC( size_t value ) { afOrderCnt_ = value; }
  void setRefAtlasListIndexInSPS( size_t listIdx ) { refAtlasListIndexInSPS_ = listIdx; }
  void setRefAFOCList( std::vector<std::vector<size_t>>& list );
  void setRefAFOCList( PCCContext& context );
  void constructAtghRefListStruct( PCCContext& context, AtlasTileGroupHeader& atgh );
  void setNumRefIdxActive( size_t value ) { numRefIdxActive_ = value; }
  void setNumRefIdxActive( PCCContext& context, AtlasTileGroupHeader& atgh );
  void addRefAFOC( size_t value ) { refAFOCList_[0].push_back( value ); }
  void addRefAFOC( size_t idx, size_t value ) { refAFOCList_[idx].push_back( value ); }

  size_t getRefAFOC( size_t refIndex ) { return refAFOCList_[0][refIndex]; }
  size_t getRefAFOC( size_t listIdx, size_t refIndex ) { return refAFOCList_[listIdx][refIndex]; }
  size_t getRefAFOCListSize( size_t idx ) { return refAFOCList_[idx].size(); }
  size_t getAtlasFrmOrderCntVal() { return atlasFrmOrderCntVal_; }
  size_t getAtlasFrmOrderCntMsb() { return atlasFrmOrderCntMsb_; }
  size_t getAtlasFrmOrderCntLsb() { return atlasFrmOrderCntLsb_; }
  size_t getNumOfRefAtlasFrameList() { return numOfAvailableRefAtlasFrameList_; }
  size_t getNumRefIdxActive() { return numRefIdxActive_; }
  size_t getActiveRefAtlasFrameIndex() { return activeRefAtlasFrameIndex_; }
  size_t getRefAtlasListIndexInSPS() { return refAtlasListIndexInSPS_; }
  size_t getPFOC() const { return afOrderCnt_; }
  void   allocOneLayerData();
  void   printBlockToPatch( const size_t occupancyResolution );
  void   printPatch();
  void   printPatchDecoder();

 private:
  size_t                                       index_;
  size_t                                       numMatchedPatches_;
  size_t                                       width_;
  size_t                                       height_;
  size_t                                       MPGeoWidth_;
  size_t                                       MPGeoHeight_;
  size_t                                       MPAttWidth_;
  size_t                                       MPAttHeight_;
  size_t                                       numberOfMissedPointsPatches_;
  size_t                                       totalNumberOfMissedPoints_;
  size_t                                       totalNumberOfEddPoints_;
  size_t                                       totalNumberOfRegularPoints_;
  size_t                                       globalPatchCount_;
  size_t                                       geometry3dCoordinatesBitdepth_;
  uint8_t                                      pointLocalReconstructionNumber_;
  bool                                         losslessGeo_;
  bool                                         losslessGeo444_;
  bool                                         useMissedPointsSeparateVideo_;
  bool                                         rawPatchEnabledFlag_;
  size_t                                       geometry2dNorminalBitdepth_;
  size_t                                       maxDepth_;
  size_t                                       bitCount2dPosXMinus1_;
  size_t                                       bitCount2dPosYMinus1_;
  size_t                                       bitCount3dPosXMinus1_;
  size_t                                       bitCount3dPosYMinus1_;
  size_t                                       afOrderCnt_;
  size_t                                       refAtlasListIndexInSPS_;
  size_t                                       numRefIdxActive_;
  size_t                                       atlasFrmOrderCntVal_;
  size_t                                       atlasFrmOrderCntMsb_;
  size_t                                       atlasFrmOrderCntLsb_;
  size_t                                       numOfAvailableRefAtlasFrameList_;
  size_t                                       activeRefAtlasFrameIndex_;
  std::vector<std::vector<size_t>>             refAFOCList_;
  size_t                                       log2PatchQuantizerSizeX_;
  size_t                                       log2PatchQuantizerSizeY_;
  std::vector<PCCVector3<size_t>>              pointToPixel_;
  std::vector<size_t>                          blockToPatch_;
  std::vector<uint32_t>                        occupancyMap_;
  std::vector<uint32_t>                        fullOccupancyMap_;
  std::vector<PCCPatch>                        patches_;
  std::vector<PCCMissedPointsPatch>            missedPointsPatches_;
  std::vector<size_t>                          numberOfMissedPoints_;
  std::vector<PCCColor3B>                      mpsTextures_;
  std::vector<PCCColor3B>                      eddTextures_;
  std::vector<PCCPointSet3>                    srcPointCloudByPatch_;
  std::vector<PCCPointSet3>                    srcPointCloudByBlock_;
  std::vector<PCCPointSet3>                    recPointCloudByBlock_;
  std::vector<std::vector<PCCVector3<size_t>>> pointToPixelByBlock_;
  PCCGPAFrameSize                              prePCCGPAFrameSize_;
  PCCGPAFrameSize                              curPCCGPAFrameSize_;
  PCCFrameOCPInfo                              ocpGPAInfo_;
  PCCVector3D                                  weightNormal_;
  std::vector<PCCEomPatch>                     eomPatches_;
};

};  // namespace pcc

#endif /* PCCFrameContext_h */

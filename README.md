# Deep Learning Geometry Compression Artifacts Removal for Video-Based Point Cloud Compression

[[bib]](http://w.web.umkc.edu/wj3wr/IJCV_bib.txt) [[pdf]](http://w.web.umkc.edu/wj3wr/IJCV.pdf)

https://link.springer.com/article/10.1007/s11263-021-01503-6

W. Jia, L. Li, Z. Li, and S. Liu, “Deep Learning Geometry Compression Artifacts Removal for Video-based Point Cloud Compression,” in International Journal of Computer Vision (IJCV), 2021

# VPCC - mpeg-pcc-tmc2 - realese 

## Building

Bash scripts can be use to build mpeg-pcc-tmc2 project: 
- build.sh: build solutions.  
- clear.sh: clear solututions.
 
### OSX
- mkdir build
- cd build
- cmake .. -G Xcode 
- open the generated xcode project and build it

### Linux
- mkdir build
- cd build
- cmake .. 
- make

### Windows
- md build
- cd build
- cmake .. -G "Visual Studio 15 2017 Win64"
- open the generated visual studio solution and build it

### External dependencies

The external dependencies must be download and build: 
- HM-16.20+SCM-8.8 (apply  pcc_me-ext_for_HM-16.20+SCM-8.8.patch on it stored in \dependencies\hm-modification ).
   
   svn checkout https://hevc.hhi.fraunhofer.de/svn/svn_HEVCSoftware/tags/HM-16.20+SCM-8.8/ external/HM-16.20+SCM-8.8-3DMC;    
   cd external/HM-16.20+SCM-8.8-3DMC
   svn patch ../../tmc2_r7.0/dependencies/hm-modification/pcc_me-ext_for_HM-16.20+SCM-8.8.patch 
   
- HDRTools
- pcc_distortion

## Running

Configuration files are provided in the cfg directory to aid configuring
the encoder.  The general pattern of usage is illustrated below, where
multiple configuration files control different aspects of the test
conditions.

NB: parameters set in one configuration file override the same parameter
in earlier files.  ie. order matters.

Further help text describing option usage is available using "./bin/PccAppEncoder --help" or "./bin/PccAppDecoder --help".

### PccAppEncoder

```
./bin/PccAppEncoder \
	--configurationFolder=cfg/ \
	--config=cfg/common/ctc-common.cfg \
	--config=cfg/condition/ctc-all-intra.cfg \
	--config=cfg/sequence/longdress_vox10.cfg \
	--config=cfg/rate/ctc-r3.cfg \
	--uncompressedDataFolder=~/mpeg_datasets/CfP/datasets/Dynamic_Objects/People/ \
	--frameCount=1 \
	--videoEncoderPath=../external/HM-16.16/bin/TAppEncoderStatic \
	--colorSpaceConversionPath=../external/HDRTools/bin/HDRConvert \
	--reconstructedDataPath=S26C03R03_rec_%04d.ply \
	--compressedStreamPath=S26C03R03.bin 
```

### PccAppDecoder

```
./bin/PccAppDecoder \
	--compressedStreamPath=S26C03R03.bin \
	--videoDecoderPath=../external/HM-16.16/bin/TAppDecoderStatic \
	--colorSpaceConversionPath=../external/HDRTools/bin/HDRConvert \ 
	--inverseColorSpaceConversionConfig=cfg/hdrconvert/yuv420torgb444.cfg \
	--reconstructedDataPath=S26C03R03_dec_%04d.ply 
```

### Scripts

More examples of running could be found in ./test/runme_linux.sh. 

These examples can be start based on your system with the following scripts:
- ./test/runme_linux.sh
- ./test/runme_windows.bat
- ./test/runme_osx.sh




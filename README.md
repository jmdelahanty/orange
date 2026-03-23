# orange :orange: 
A multi-camera capture, streaming and recording GUI application for emergent cameras in C++

Contact [Jinyao Yan](yanj11@janelia.hhmi.org) if you have questions about the software 

![gui](images/gui.png)

## Video demo 
Please see this [link](https://youtu.be/ahceluqBYj8) for a video demo of the app. 

## Features 
1. Multiple cameras streaming 
2. PTP synchronization 
3. GPU accelerated encoding (h264, h265)
3. Support mono and color Emergent cameras
4. Multiple servers communication

## Benchmark
Encoding performance using GPU A6000 with 7MP Emergent camera

![encoding_benchmark](images/encoding_benchmark.png)

## Dependencies
1. Emergent SDK
2. CUDA Toolkit
3. FFmpeg 
4. OpenCV 
5. OpenGL and GLEW 
6. DearImGUI and related repos
7. TensorRT 
8. ENET

## Build instructions 
0. If you wish to skip the build process, an Ubuntu image is available with preinstalled `orange` and the labeling app `red`. Please contact the developer for accessing the image and follow instructions [here](docs/clonezilla_image.md). 

Build-system migration plan/status:
[`docs/build_system_migration_todo.md`](docs/build_system_migration_todo.md)

1. Install CUDA (the software has been tested with version 12.x) and Emergent camera SDK. Follow instructions in [`docs/install_linux_cuda_eSDK.md`](docs/install_linux_cuda_eSDK.md). Make sure you can stream all cameras individually with Emergent `eCapture`.  

2. Install FFmpeg 4.4

Refer to [`docs/install_ffmpeg.md`](docs/install_ffmpeg.md) for detailed instruction for building FFmpeg 4.4. 

For the CMake build, you can override dependency roots with cache variables such as
`-DORANGE_FFMPEG_ROOT=/path/to/ffmpeg/build` and
`-DORANGE_TENSORRT_ROOT=/path/to/TensorRT`.

3. Install OpenGL and GLEW
```
sudo apt-get install libglfw3
sudo apt-get install libglfw3-dev
sudo apt-get install libglew-dev
```

4. Install OpenCV
Refer to [`docs/install_opencv.md`](docs/install_opencv.md) for detailed instruction for building OpenCV. 

5. Install TensorRT 
Followings instruction: [`docs/install_tensorrt.md`](docs/install_tensorrt.md). If TensorRT is installed at a non-default location, pass `-DORANGE_TENSORRT_ROOT=/path/to/TensorRT` when configuring CMake.

6. Install ENET
Follow instruction: http://enet.bespin.org/Installation.html. You might need to run `sudo ldconfig` in terminal after installation, or simply reboot your computer. 

7. Clone the repo and submodules

```
git clone --recursive https://github.com/JohnsonLabJanelia/orange.git
```

8. Build with CMake. Common preset-driven app builds are:
```
cmake --preset release
cmake --build --preset release
```

For a debug + NVTX build:
```
cmake --preset debug_nvtx
cmake --build --preset debug_nvtx
```

The legacy [`build.sh`](build.sh) script is now a thin compatibility wrapper around CMake, so the old workflow still works:
```
./build.sh
./build.sh --debug --nvtx
```

The application binary is written under `targets/<variant>/orange`. The run script will look for a compatible binary automatically:
```
./run.sh
```

If you want to run a specific binary directly:
```
ORANGE_BIN=./targets/debug_nvtx/orange ./run.sh
```

## Lens diagnostics (EVT EF/UART)
For EF mount focus troubleshooting, use the standalone probe utility:

- Runbook and commands: [`docs/evt_lens_probe.md`](docs/evt_lens_probe.md)
- Investigation checklist: [`docs/evt_ef_lens_focus_investigation_todo.md`](docs/evt_ef_lens_focus_investigation_todo.md)
- Post-patch validation steps: see `Validation Procedure After App Patch` in [`docs/evt_lens_probe.md`](docs/evt_lens_probe.md)
- Per-camera control: set `focus_uart_bootstrap` in camera JSON to explicitly enable/disable UART focus bootstrap.

## Use the Application
When first time open the program, `orange` creates folders with the following structure

```
orange_data
├── config
│   ├── local
│   └── network
├── detect
├── exp
│   └── unsorted
└── pictures

```

The are two modes of using the application: local vs network. Local means all cameras are connected to one server, while network can support multiple servers. 

### Local mode
One could save preconfigued camera settings in a folder under `local`, for instance

```
orange_data
├── config
│   ├── local
│   │   ├── 5cam
│   │   │   ├── 2002488.json
│   │   │   ├── 2002489.json
│   │   │   ├── 2002490.json
│   │   │   ├── 2002496.json
│   │   │   └── 710038.json
│   │   └── center_ceiling
│   │       └── 710038.json
│   └── network
├── detect
│   └── rat_bbox.engine
├── exp
│   └── unsorted
│       └── 2024_10_31_13_09_41
│           ├── Cam710038_meta.csv
│           └── Cam710038.mp4
└── pictures
    └── 710038_0.tiff

```
In the node folder (like `5cam` folder), it contains 1 or more camera configs `[camera serial].json`. An example config file is in the `config` folder. Please name the file after the serial number of your cameras and set the config according to your camera specifications. To enable `gpu_direct`, set `gpu_direct` to true, and set the `gpu_id` to select which gpu to use for image processing of the camera. 

### Network mode
One can network multiple PCs to scale up to more cameras. 

The recorded videos are saved at `orange_data/exp/unsorted` by default. But it can be easily changed while using the app. 

### PTP setting
Please refer to [`docs/ptp.md`](docs/ptp.md) for detailed instruction for configure PTP. 
For local reliability, use the helper script documented there: `scripts/ptp_stack.sh`.


## Contribute

Please open an issue for bug fix or feature request. If you wish to make changes to the source code, you can fork the repo. To contribute to the project, please create a [pull request](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request).

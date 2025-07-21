#!/bin/bash
DIR="$(dirname "$(realpath "$0")")"
PARENT_DIR="$(dirname "$DIR")"
cd "$PARENT_DIR"
targets_folder="$PARENT_DIR/targets"

mkdir -p "$targets_folder"
rm -f "$targets_folder/orange"
nvcc -c src/kernel.cu -arch=sm_80 -o "$targets_folder/kernel.o"

# ──────────────────────────────
# Updated directory variables
# ──────────────────────────────
ORANGE_ROOT="/opt/orange"
DIR_FFMPEG="${ORANGE_ROOT}/lib/ffmpeg-nvidia"
DIR_TENSORRT="/usr/local/TensorRT-10.0.1.6"
DIR_OPENCV="${ORANGE_ROOT}/lib/opencv"

DIR_IMGUI="third_party/imgui"
DIR_IMGUI_BACKEND="third_party/imgui/backends"
DIR_IMPLOT="third_party/implot"
DIR_FILEBROWSER="third_party/ImGuiFileDialog"
DIR_ICONFONT="third_party/IconFontCppHeaders"

# ──────────────────────────────
# ImGui / ImPlot object files
# ──────────────────────────────
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui.o" "$DIR_IMGUI/imgui.cpp"
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui_demo.o" "$DIR_IMGUI/imgui_demo.cpp"
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui_draw.o" "$DIR_IMGUI/imgui_draw.cpp"
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui_tables.o" "$DIR_IMGUI/imgui_tables.cpp"
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui_widgets.o" "$DIR_IMGUI/imgui_widgets.cpp"
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui_impl_glfw.o" "$DIR_IMGUI_BACKEND/imgui_impl_glfw.cpp"
g++ -std=c++11 -I"$DIR_IMGUI" -I"$DIR_IMGUI_BACKEND" -g -Wall -Wformat $(pkg-config --cflags glfw3) \
    -c -o "$targets_folder/imgui_impl_opengl3.o" "$DIR_IMGUI_BACKEND/imgui_impl_opengl3.cpp"

g++ -std=c++17 -I"$DIR_IMPLOT" -I"$DIR_IMGUI" -g -Wall -c -o "$targets_folder/implot.o" "$DIR_IMPLOT/implot.cpp"
g++ -std=c++17 -I"$DIR_IMPLOT" -I"$DIR_IMGUI" -g -Wall -c -o "$targets_folder/implot_items.o" "$DIR_IMPLOT/implot_items.cpp"
g++ -std=c++17 -I"$DIR_IMPLOT" -I"$DIR_IMGUI" -g -Wall -c -o "$targets_folder/implot_demo.o" "$DIR_IMPLOT/implot_demo.cpp"

# ──────────────────────────────
# Final link
# ──────────────────────────────
g++ -Ofast -ffast-math -std=c++17 "$targets_folder"/*.o \
    -o "$targets_folder/orange" \
    -I ./src/ \
      src/orange.cpp src/project.cpp src/FrameSaver.cpp src/FrameDetector.cpp src/realtime_tool.cpp \
      src/detect3d.cpp src/network_base.cpp src/global.cpp src/FFmpegWriter.cpp src/camera.cpp \
      src/video_capture.cpp src/offthreadmachine.cpp src/opengldisplay.cpp src/threadworker.cpp \
      src/gpu_video_encoder.cpp src/yolov8_det.cpp "$DIR_FILEBROWSER/ImGuiFileDialog.cpp" \
    -I"$DIR_IMGUI" \
    -I"$DIR_IMGUI_BACKEND" \
    -I"$DIR_IMPLOT" \
    -I"$DIR_FILEBROWSER" \
    -I"$DIR_ICONFONT" \
    -I./src/NvEncoder/ ./src/NvEncoder/*.cpp \
    -I./nvenc_api/include -I/opt/EVT/eSDK/include/ -I/usr/local/cuda/include \
    -L/opt/EVT/eSDK/lib/ -lEmergentCamera -lEmergentGenICam -lEmergentGigEVision \
    -lm -lpthread \
    -I./third_party/flatbuffers/include -lenet -I/usr/local/include/ \
    -L/usr/local/cuda/lib64/ -lcudart -lcuda -lnppicc -lnppidei -lnvidia-encode -lnppc -lnppig -lnppial \
    -lGLEW -lGL \
    -I"$DIR_FFMPEG/include" \
    -L"$DIR_FFMPEG/lib" -lavformat -lswscale -lswresample -lavutil -lavcodec \
    -I/usr/local/include/opencv4 \
    -L"$DIR_OPENCV/lib" -lopencv_sfm -lopencv_core -lopencv_imgcodecs -lopencv_imgproc -lopencv_calib3d \
    -I"$DIR_TENSORRT/include" \
    -L"$DIR_TENSORRT/lib" -lnvinfer -lnvinfer_plugin \
    $(pkg-config --static --libs glfw3)

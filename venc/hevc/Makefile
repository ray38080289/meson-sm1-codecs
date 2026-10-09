# out-of-tree build: make -C <kernel build dir> M=$PWD modules
obj-m += amvenc_hevc.o
amvenc_hevc-y := vpu.o compat.o w4enc.o venc_v4l2.o
ccflags-y += -Wno-unused-function -Wno-missing-prototypes

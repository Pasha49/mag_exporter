#!/bin/bash

sudo docker run --rm -v "$PWD":/usr/src/app mag-c-builder sh4-linux-gcc -O3 -static main.c -o mag_exporter && \
    sudo docker run --rm -v "$PWD":/usr/src/app mag-c-builder sh4-linux-strip mag_exporter

#!/bin/bash

for arg in "$@"; do
  case $arg in
    --clay|--clay-*)
      CLAY=${arg#--clay-}
      shift
      ;;
    --sdl2)
      SDL2=true
      shift
      ;;
    --sdl2-ttf)
      SDL2_TTF=true
      shift
      ;;
    --sdl2-image)
      SDL2_IMAGE=true
      shift
      ;;
    --curl)
      CURL=true
      shift
      ;;
    --all)
      CLAY=true
      CURL=true
      SDL2=true
      SDL2_TTF=true
      SDL2_IMAGE=true
      shift
      ;;
    *)
      echo "Unknown argument: $arg"
      exit 1
      ;;
  esac
done

print_banner() {
  local msg="$1"
  local total_width=60
  local pad_char='-'
  local edge_char='#'

  local padding=$(( (total_width - ${#msg} - 2) / 2 ))
  local pad=$(printf "%${padding}s" "" | tr ' ' "$pad_char")

  echo
  echo "$edge_char $pad $msg $pad $edge_char"
}

download_from() {
    local url=$1
    shift
    local args=$@
    echo "Download from $url"
    wget ${args:--nc} $url
}

# ------------------------------------------
# CLAY
# ------------------------------------------

if [ ! -z $CLAY ]; then
    CLAY_VERSION=${CLAY_VERSION:-0.13}
    CLAY_RELEASE=https://github.com/nicbarker/clay/releases/download/v$CLAY_VERSION/clay.h
    CLAY_MAIN=https://raw.githubusercontent.com/nicbarker/clay/refs/heads/main/clay.h

    CLAY_URL=$CLAY_MAIN
    if [ "$CLAY" == "main" ]; then
        CLAY_URL=$CLAY_MAIN
    elif [ "$CLAY" == "release" ]; then
        CLAY_URL=$CLAY_RELEASE
    fi

    print_banner "Downloading CLAY"
    download_from "$CLAY_URL" --output-document clay.h
fi

# ------------------------------------------
# SDL2
# ------------------------------------------

mkdir -p __tmp__ build external
cd __tmp__

SDL2_ARCH=x86_64-w64-mingw32 #x86_64-w64-mingw32 #i686-w64-mingw32
SDL2_VERSION=2.32.4
SDL2_RELEASE=https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.zip

SDL2_TTF_VERSION=2.24.0
SDL2_TTF_RELEASE=https://github.com/libsdl-org/SDL_ttf/releases/download/release-${SDL2_TTF_VERSION}/SDL2_ttf-devel-${SDL2_TTF_VERSION}-mingw.zip

SDL2_IMAGE_VERSION=2.8.8
SDL2_IMAGE_RELEASE=https://github.com/libsdl-org/SDL_image/releases/download/release-${SDL2_IMAGE_VERSION}/SDL2_image-devel-${SDL2_IMAGE_VERSION}-mingw.zip

[ "$SDL2" == "true" ]       && print_banner "Downloading SDL2"       && download_from $SDL2_RELEASE
[ "$SDL2_TTF" == "true" ]   && print_banner "Downloading SDL2 TTF"   && download_from $SDL2_TTF_RELEASE
[ "$SDL2_IMAGE" == "true" ] && print_banner "Downloading SDL2_IMAGE" && download_from $SDL2_IMAGE_RELEASE

[ "$SDL2" == "true" ]       && print_banner "Unpacking SDL2"       && unzip -n SDL2-devel-${SDL2_VERSION}-mingw.zip
[ "$SDL2_TTF" == "true" ]   && print_banner "Unpacking SDL2 TTF"   && unzip -n SDL2_ttf-devel-${SDL2_TTF_VERSION}-mingw.zip
[ "$SDL2_IMAGE" == "true" ] && print_banner "Unpacking SDL2_IMAGE" && unzip -n SDL2_image-devel-${SDL2_IMAGE_VERSION}-mingw.zip

[ "$SDL2" == "true" ]       && cp ./SDL2-${SDL2_VERSION}/${SDL2_ARCH}/bin/SDL2.dll                    ../build/
[ "$SDL2_TTF" == "true" ]   && cp ./SDL2_ttf-${SDL2_TTF_VERSION}/${SDL2_ARCH}/bin/SDL2_ttf.dll        ../build/
[ "$SDL2_IMAGE" == "true" ] && cp ./SDL2_image-${SDL2_IMAGE_VERSION}/${SDL2_ARCH}/bin/SDL2_image.dll  ../build/

[ "$SDL2" == "true" ]       && cp -r ./SDL2-${SDL2_VERSION}/${SDL2_ARCH}/{include,lib}                ../external/
[ "$SDL2_TTF" == "true" ]   && cp -r ./SDL2_ttf-${SDL2_TTF_VERSION}/${SDL2_ARCH}/{include,lib}        ../external/
[ "$SDL2_IMAGE" == "true" ] && cp -r ./SDL2_image-${SDL2_IMAGE_VERSION}/${SDL2_ARCH}/{include,lib}    ../external/

# ------------------------------------------
# CURL
# ------------------------------------------

if [ ! -z $CURL ]; then
echo
    CURL_VERSION=8.15.0_5
    CURL_RELEASE=https://curl.se/windows/dl-$CURL_VERSION/curl-$CURL_VERSION-win64-mingw.zip

    print_banner "Downloading CURL" && download_from "$CURL_RELEASE"
    print_banner "Unpacking CURL"   && unzip -n curl-$CURL_VERSION-win64-mingw.zip

    cp ./curl-$CURL_VERSION-win64-mingw/bin/libcurl-x64.dll   ../build/
    cp -r ./curl-$CURL_VERSION-win64-mingw/{include,lib}      ../external/
fi
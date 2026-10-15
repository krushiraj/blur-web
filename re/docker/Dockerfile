FROM --platform=linux/amd64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive WINEARCH=win32 WINEPREFIX=/wine WINEDEBUG=-all DISPLAY=:99
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
    wine wine32:i386 wine64 xvfb xauth x11-utils cabextract winbind ca-certificates wget \
    libgl1-mesa-dri libgl1-mesa-dri:i386 mesa-vulkan-drivers xdotool imagemagick tcpdump \
 && rm -rf /var/lib/apt/lists/*
RUN wget -q -O /usr/local/bin/winetricks https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks && chmod +x /usr/local/bin/winetricks
COPY run.sh /run.sh
RUN chmod +x /run.sh

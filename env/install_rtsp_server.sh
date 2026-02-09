#!/bin/bash
set +e

VERSION="v1.11.2"

TAR_GZ_FILE="mediamtx_${VERSION}_linux_amd64.tar.gz"
WORKING_DIR="/tmp/$(uuidgen)"
echo "Working directory: ${WORKING_DIR}"
mkdir "${WORKING_DIR}"
cd "${WORKING_DIR}"
wget https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/${TAR_GZ_FILE}
tar xfvz "${TAR_GZ_FILE}"
sudo mv mediamtx /usr/local/bin/
sudo mkdir /etc/mediamtx
sudo cp mediamtx.yml /etc/mediamtx/

cat <<EOF | sudo tee mediamtx.service > /dev/null
[Unit]
Description=MediaMTX RTSP Server
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/mediamtx /etc/mediamtx/mediamtx.yml
Restart=always

[Install]
WantedBy=multi-user.target
EOF
sudo mv mediamtx.service /etc/systemd/system/mediamtx.service

sudo systemctl daemon-reload
sudo systemctl start mediamtx
sudo systemctl enable mediamtx
echo "The server will now be running on the default RTSP port (8554)."
echo "You can stream to it using:"
echo "rtsp://your-server-ip:8554/stream-name"
echo "To test the server, you can use FFmpeg to publish a test stream:"
echo "ffmpeg -re -f lavfi -i testsrc -f rtsp rtsp://localhost:8554/test"
echo "To view the stream, you can use VLC or FFplay:"
echo "ffplay rtsp://localhost:8554/test"

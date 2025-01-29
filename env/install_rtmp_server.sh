#!/bin/bash

# Exit on any error
set -e

# Function to check if script is run as root
check_root() {
    if [ "$EUID" -ne 0 ]; then 
        echo "Please run as root (use sudo)"
        exit 1
    fi
}

# Function to create nginx configuration
create_nginx_conf() {
    cat > /usr/local/nginx/conf/nginx.conf << 'EOF'
worker_processes auto;

events {
    worker_connections 1024;
}

rtmp {
    server {
        listen 1935;
        chunk_size 4096;

        application live {
            live on;
            record off;
        }
    }
}

http {
    include       mime.types;
    default_type  application/octet-stream;

    server {
        listen 80;
        
        location /stat {
            rtmp_stat all;
            rtmp_stat_stylesheet stat.xsl;
        }
    }
}
EOF
}

# Function to create systemd service
create_systemd_service() {
    cat > /etc/systemd/system/nginx.service << 'EOF'
[Unit]
Description=nginx - high performance web server
Documentation=http://nginx.org/en/docs/
After=network-online.target remote-fs.target nss-lookup.target
Wants=network-online.target

[Service]
Type=forking
ExecStart=/usr/local/nginx/sbin/nginx
ExecReload=/usr/local/nginx/sbin/nginx -s reload
ExecStop=/usr/local/nginx/sbin/nginx -s stop
RestartSec=10s

[Install]
WantedBy=multi-user.target
EOF
}

# Main installation function
install_rtmp() {
    echo "Starting RTMP server installation..."
    
    # Update system and install dependencies
    apt update
    apt install -y build-essential libpcre3 libpcre3-dev libssl-dev zlib1g-dev wget unzip

    # Create temporary directory
    TEMP_DIR=$(mktemp -d)
    cd "$TEMP_DIR"

    # Download and extract Nginx and RTMP module
    echo "Downloading Nginx and RTMP module..."
    wget -q http://nginx.org/download/nginx-1.24.0.tar.gz
    wget -q https://github.com/arut/nginx-rtmp-module/archive/master.zip
    
    tar -xf nginx-1.24.0.tar.gz
    unzip -q master.zip

    # Configure and compile Nginx with RTMP module
    echo "Configuring and compiling Nginx..."
    cd nginx-1.24.0
    ./configure --with-http_ssl_module --add-module=../nginx-rtmp-module-master
    make -j$(nproc)
    make install

    # Create configuration
    echo "Creating Nginx configuration..."
    create_nginx_conf

    # Create systemd service
    echo "Setting up systemd service..."
    create_systemd_service

    # Configure firewall
    if command -v ufw >/dev/null 2>&1; then
        echo "Configuring firewall..."
        ufw allow 1935
        ufw allow 80
    fi

    # Start and enable service
    systemctl daemon-reload
    systemctl enable nginx
    systemctl start nginx

    # Clean up
    cd
    rm -rf "$TEMP_DIR"

    # Get server IP
    SERVER_IP=$(hostname -I | awk '{print $1}')

    echo "Installation completed successfully!"
    echo "Your RTMP server is ready to use:"
    echo "Streaming URL: rtmp://${SERVER_IP}/live"
    echo "Add your stream key after /live/ when streaming"
    echo "Example: rtmp://${SERVER_IP}/live/mystream"
    echo ""
    echo "To check server status: systemctl status nginx"
    echo "To stop server: systemctl stop nginx"
    echo "To start server: systemctl start nginx"
    echo "To restart server: systemctl restart nginx"
    echo ""
    echo "Configuration file location: /usr/local/nginx/conf/nginx.conf"
}

# Check if running as root
check_root

# Start installation
install_rtmp

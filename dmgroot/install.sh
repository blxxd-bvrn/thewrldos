#!/bin/sh
# Installs Lumina Demo.app from this disk image into C:\Program Files
src="$(dirname "$0")/Lumina Demo.app"
echo "Installing Lumina Demo from $src ..."
mkdir -p "C:/Program Files"
cp "$src" "C:/Program Files/"
echo "C:\\Program Files\\Lumina Demo.app" > "C:/Documents and Settings/User/Desktop/Lumina Demo.lnk"
echo -e "\e[92mDone.\e[0m A shortcut was placed on your Desktop."

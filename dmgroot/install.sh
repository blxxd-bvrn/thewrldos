#!/bin/sh
# Installs WRLD Demo.app from this disk image into C:\Program Files
src="$(dirname "$0")/WRLD Demo.app"
echo "Installing WRLD Demo from $src ..."
mkdir -p "C:/Program Files"
cp "$src" "C:/Program Files/"
echo "C:\\Program Files\\WRLD Demo.app" > "C:/Documents and Settings/User/Desktop/WRLD Demo.lnk"
echo -e "\e[92mDone.\e[0m A shortcut was placed on your Desktop."

# ai_camera

A lightweight replacement for the stock ai_camera service on the Elegoo Centauri Carbon 2. Supports higher resolution streaming and timelapses while (hopefully) being less prone to crashing.

---

## Use

0. **Back up the original ai_camera**:
   ```bash
   root@TinaLinux:~# cp /opt/bin/ai_camera /opt/bin/ai_camera.bak
   ```

1. **Compile**:
   ```bash
   [you@host ai_camera]$ make
   ```

2. **Stop the printer service**:
   ```bash
   root@TinaLinux:~# /etc/init.d/printer stop
   ```

3. **Copy executable to printer**:
   ```bash
   [you@host ai_camera]$ scp ai_camera root@[IP_ADDRESS]:/opt/bin/ai_camera
   ```

4. **Reboot printer**:
   ```bash
   root@TinaLinux:~# reboot
   ```

---

## Open issues

- Timelapses might appear on the touchscreen UI but not in any web UIs. I do not know why this happens.
- All "AI" functionality is removed. This is intentional.
- Needs more stress testing
- Unknown if higher resolutions can overwhelm the USB bus
- Make system may or may not work on your machine

---

## Warnings

- Only tested in LAN only mode
- Not tested on all firmware versions
- Only tested on Elegoo Centauri Carbon 2
- Author is not responsible for any damage caused by this software
- Could eat your dog

---

## Acknowledgements

This project is made possible thanks to the following open-source libraries:

- **[x264](https://code.videolan.org/videolan/x264)**
- **[minimp4](https://github.com/lieff/minimp4)**
- **[stb_image](https://github.com/nothings/stb)**
- **[musl libc](https://musl.libc.org/)** & **[musl-cross](https://musl.cc/)**

---

## License

This project is licensed under AGPLv3. Elegoo is not exempt from the GPL no matter how much they pretend otherwise.


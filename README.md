# Linux driver for Elgato Game Capture HD60 S

This is a reverse-engineered Linux driver for the Elgato Game Capture HD60 S USB 3.0 HDMI video capture device, which was never officially supported in Linux.

## AI Disclosure

This project was largely made possible through the use of AI. Specifically, Claude Opus 5 and Fable 5 were both involved in the reverse engineering analysis and writing the driver. Register reads/writes and USB protocol behavior were derived from USB captures, device firmware, and static analysis of the Windows driver. Timing tables were also extracted from the Windows driver. I did do a lot of reverse engineering by hand on the MCU firmware originally for [my blog post about fixing the LEDs](https://www.downtowndougbrown.com/2024/09/fixing-an-elgato-hd60-s-hdmi-capture-device-with-the-help-of-ghidra/), but it was not nearly enough to figure out every little detail of how the HD60 S works. Despite the heavy involvement of AI, this documentation is entirely handwritten.

## Introduction

There are two different generations of HD60 S devices. Both generations share much of the same architecture: a Cypress CYUSB3014 FX3 USB 3.0 peripheral controller, a Nuvoton Cortex-M0 microcontroller, an Altera MAX II CPLD, and an ITE IT66121 HDMI transmitter IC. All devices use USB vendor ID 0x0FD9. Although the two generations look the same on the outside, they are significantly different on the inside:

- **First generation:** USB PIDs `0x004F`, `0x005E`, and `0x0074`. These are revisions 1-3, and they have an MStar MST3367 HDMI receiver IC. The driver is expected to manually set up the HDMI receiver and transmitter ICs.
- **Second generation:** USB PID `0x0076`. This is revision 4, which uses an ITE IT6802E HDMI receiver IC instead. On this revision, the microcontroller is in charge of configuring the HDMI receiver and transmitter ICs, and the driver just performs some simple setup.

For that reason, this driver essentially is responsible for configuring two completely different products that happen to share some common USB transport logic. Revisions 1-3 are significantly more complicated to support than revision 4, particularly because of the HDMI RX/TX ICs needing to be manually configured.

Both generations of devices have been extensively tested during development of this driver. Video capture of resolutions all the way from 480i up to 1080p60 is working, and audio capture is also working.

## Requirements

For now I'm officially supporting versions back to kernel v5.10. Feel free to submit a PR if you get it working with earlier versions. Testing was performed in Ubuntu 24.04 with kernel v6.8 and v7.0.

## How to build and use

As long as `/lib/modules/<your kernel version>/build` exists (make sure your kernel headers are installed with something like `sudo apt install linux-headers-generic` or the equivalent HWE package if you're running an HWE kernel), it should be possible to build with a simple command:

`make`

And then insert the module:

```
sudo modprobe -a v4l2-dv-timings videodev videobuf2-vmalloc videobuf2-v4l2 snd-pcm
sudo insmod hd60s.ko
```

Note that if you have Secure Boot turned on, you will get an error about the key being rejected by the service. My recommendation is just to leave Secure Boot turned off if you are playing with this driver. Otherwise, there is a whole process you can follow to enroll a key using mokutil and then sign the hd60s.ko module after compiling it.

## Important info about resolution changes

The HD60 S is unlike a lot of other capture devices that provide scaling. With most consumer USB capture devices, such as devices that use the generic UVC driver, you pick a capture resolution and the HDMI input will be scaled to whatever size you ask. On the other hand, the HD60 S doesn't allow you to pick a target size. It only offers a direct output stream of the resolution it has detected.

This leads to situations like the following kernel message you will see if you change the captured HDMI source from 720p to 1080p:

`source changed to 1920x1080 while streaming 1280x720, frames stop until the capture is restarted`

OBS doesn't know how to handle this situation. The message implies that all you have to do is stop and restart the capture, but you will find that it won't actually fix the problem. You'll just get this message instead:

`source is 1920x1080 but the capture is 1280x720, frames stop until the new timings are set`

You can fix this by running this command to select the new timing (assuming your device is /dev/video0):

`v4l2-ctl -d /dev/video0 --set-dv-bt-timings query`

So in summary, to work around this problem, close the capture device, run the command above, and then reopen the capture device after a resolution change.

My understanding is that this is typical V4L2 device behavior in Linux, and this driver is handling the situation correctly. For example, GStreamer handles resolution changes with this driver automatically and seamlessly. Here's a sample pipeline:

`gst-launch-1.0 v4l2src device=/dev/video0 ! videoconvert ! autovideosink`

I believe this is ultimately a problem with OBS that people typically don't encounter because most consumer USB capture cards used in Linux have a scaler. Maybe OBS can be changed to add behavior similar to GStreamer.

## Audio input

Here are sample commands for choosing the audio input (assuming the device is /dev/video0):

- Set to HDMI audio: `v4l2-ctl -d /dev/video0 --set-audio-input=0`
- Set to 3.5mm audio jack: `v4l2-ctl -d /dev/video0 --set-audio-input=1`

The 3.5mm audio jack is currently untested. I don't have anything handy to test it. Let me know if it works!

## Isochronous vs. bulk transport

There is a `force_bulk` parameter you can optionally specify when inserting the module:

`sudo insmod hd60s.ko force_bulk=1`

Omit it for most uses. The driver will typically use isochronous transfers by default, which usually makes sense. Set it to 1 if you want to force bulk transfers, which might help with buggy host controllers or if you have an unreliable USB link that needs the error correction that bulk mode provides.

## Still TODO

- The 3.5mm audio input jack is untested.
- The entire driver likely needs a lot more testing with different people's setups.

## Credits

- [usb3hdcap](https://github.com/mlaux/usb3hdcap) was a helpful reference for MST3367 register info.
- [stoth68000](https://github.com/stoth68000/hdcapm) was helpful for the very same reason.
- [Time Sleuth](https://github.com/chriz2600/time-sleuth) was instrumental for testing different capture resolutions.
- Elgato's Windows CY3014.X64.SYS driver was extremely helpful for analysis purposes.

## License

GPL-2.0. See [LICENSE.txt](LICENSE.txt) for the full text.

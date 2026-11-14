# udev rule (non-root access to the controller)

Reading and writing the profile needs access to the device's `hidraw` node.
Install the rule and reload:

```bash
sudo tee /etc/udev/rules.d/72-8bitdo.rules >/dev/null <<'RULE'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2dc8", TAG+="uaccess"
SUBSYSTEM=="usb",    ATTRS{idVendor}=="2dc8", TAG+="uaccess"
SUBSYSTEM=="input",  ATTRS{idVendor}=="2dc8", TAG+="uaccess"
RULE
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw
```

`TAG+="uaccess"` grants access to the user of the active session only.
Replug the controller after installing the rule.

Verify with:

```bash
getfacl -p /dev/hidraw0 | grep user
lsusb | grep 2dc8
```

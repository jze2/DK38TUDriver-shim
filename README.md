# DK38TUDriver-shim
Wrapper around DK38TUDriver library translating new PCSC API calls to the driver's old.

I have an old Dekart SIM card reader with 32bit only driver that no longer works due to its deprecated PCSC API.
So here is a wrapper that translates the new PCSC API calls to the driver's old one.

Build with
```
gcc -m32 -Wall -Wextra -std=c11 -fPIC -shared -o DK38TUDriver.so DK38TUDriver.c -ldl
```
Then rename the original `/usr/lib32/pcsc/drivers/DK38TUDriver.bundle/Contents/Linux/DK38TUDriver.so` to `DK38TUDriver.real.so` and copy the compiled wrapper in its place.

Briefly tested on EndeavourOS by running `pcscd-32 --foreground --debug --apdu` and reading SIM contacts from a SIM.

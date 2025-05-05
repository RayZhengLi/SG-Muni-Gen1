# SG-Muni-Gen1
This is the host app for Muni-kit Gen1 upgrade.
## Environment
- LMU5541-SDK & CalAmp 
- Virtual mechine Ubuntu 16.04
## How to compile
### **`Build SDK`** : 
```bash
cd lmu5541_sdk
cd calamp_env/lmu5541 && ./setup.sh && cd ../..
echo lmu5541 > .common_build_env
make dirclean
make V=s -j4
```
### **`Compile`** :  
Select the helloworld package to build. It is in the [utility] section
```bash
make menuconfig
```
Save and exit the menuconfig, and use make command:
```bash
make package/sg-muni/{clean,compile} V=s
```
Then the compiled package will be in the folder:
```
/home/ray/lmu5541_sdk/bin/lmu5541/packages/sg-muni_0.1.0-1_lmu5541.ipk
```
## How to install
SSH to the LMU and input:
```bash
opkg install sg-muni_0.1.0-1_lmu5541.ipk
```
Then reboot the LMU
### How to uninstall
In the SSH terminal of LMU, input:
```bash
/etc/init.d/sg-tcp stop
/etc/init.d/sg-tcp disable
opkg remove sg-tcp
```
Then delete the configuration files and reboot LMU
# !/bin/sh
make
sudo cp drivers/md/raid456.ko /lib/modules/3.18.8/kernel/drivers/md/
sudo make install

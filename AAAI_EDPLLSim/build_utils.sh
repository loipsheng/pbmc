cd utils/

tar -xvf m4-1.4.19.tar.xz
cd m4-1.4.19/
./configure
make
sudo make install
cd ../

tar -xvf gmp-6.2.0.tar.xz
cd gmp-6.2.0/
./configure
make
sudo make install
cd ../

tar -xzvf glpk-4.65.tar.gz
cd glpk-4.65/
./configure
make
sudo make install
sudo ldconfig
cd ../

cd ../

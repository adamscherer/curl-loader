mkdir obj

# Modify /etc/yum.repos.d/epel.repo. Under the section marked [epel], change enabled=0 to enabled=1.

sudo yum update
sudo yum install gcc
sudo yum install patch
sudo yum install openssl
sudo yum install openssl-devel
sudo yum install hiredis-devel
sudo yum install nodejs npm

export OPENSSLDIR=/usr/include/openssl

make optimize=1 debug=0
sudo make install

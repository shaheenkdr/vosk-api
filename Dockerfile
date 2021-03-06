FROM ubuntu:18.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        g++ \
        make \
        automake \
        autoconf \
        bzip2 \
        unzip \
        wget \
        sox \
        libtool \
        git \
        subversion \
        python2.7 \
        python3 \
	python3-dev \
        python3-websockets \
        python3-setuptools \
	python3-wheel \
        zlib1g-dev \
        ca-certificates \
        gfortran \
        patch \
        ffmpeg \
        nano \
        swig \
        python3-pip \
	vim && \
    rm -rf /var/lib/apt/lists/*

RUN ln -s /usr/bin/python2.7 /usr/bin/python 

RUN git clone https://github.com/shaheenkdr/kaldi.git /opt/kaldi && \
    cd /opt/kaldi && \
    cd /opt/kaldi/tools && \
    ./extras/install_mkl.sh && \
    make -j $(nproc) && \
    cd /opt/kaldi/src && \
    ./configure --shared && \
    make depend -j $(nproc) && \
    make -j $(nproc)
    
 RUN git clone --depth 1 https://github.com/shaheenkdr/vosk-api.git /opt/vosk-api && \
     export KALDI_ROOT=/opt/kaldi && \
     cd /opt/vosk-api/python/ && \
     pip3 install setuptools && \
     pip3 install websockets --upgrade && \
     python3 setup.py install --user --single-version-externally-managed --root=/

WORKDIR /home/


EXPOSE 5000

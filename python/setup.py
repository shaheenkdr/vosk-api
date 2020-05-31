import os
import sys
import setuptools
from setuptools import Extension
from setuptools.command.build_py import build_py as _build_py
import distutils.dir_util
import distutils.log

class build_py(_build_py):
    def run(self):
        self.run_command("build_ext")
        return super().run()

with open("README.md", "r") as fh:
    long_description = fh.read()

source_path=os.getenv("VOSK_SOURCE", os.path.abspath(os.path.join(os.path.abspath(os.path.dirname(__file__)), "../src")))
distutils.log.set_verbosity(distutils.log.DEBUG)
distutils.dir_util.copy_tree(
    source_path,
    "vosk",
    update=1,
    verbose=1)

kaldi_root = '/opt/kaldi'
kaldi_static_libs = ['src/online2/kaldi-online2.a',
             'src/decoder/kaldi-decoder.a',
             'src/ivector/kaldi-ivector.a',
             'src/gmm/kaldi-gmm.a',
             'src/nnet3/kaldi-nnet3.a',
             'src/tree/kaldi-tree.a',
             'src/feat/kaldi-feat.a',
             'src/lat/kaldi-lat.a',
             'src/lm/kaldi-lm.a',
             'src/hmm/kaldi-hmm.a',
             'src/transform/kaldi-transform.a',
             'src/cudamatrix/kaldi-cudamatrix.a',
             'src/matrix/kaldi-matrix.a',
             'src/fstext/kaldi-fstext.a',
             'src/util/kaldi-util.a',
             'src/base/kaldi-base.a',
             'tools/openfst/lib/libfst.a',
             'tools/openfst/lib/libfstngram.a']
kaldi_link_args = ['-s']
kaldi_libraries = []

lx = [kaldi_root + '/' + x for x in kaldi_static_libs]

lx.append('/opt/intel/mkl/lib/intel64/libmkl_rt.so')

sources = ['kaldi_recognizer.cc', 'model.cc', 'spk_model.cc', 'vosk_api.cc', 'vosk.i']

vosk_ext = Extension('vosk._vosk',
                    define_macros = [('FST_NO_DYNAMIC_LINKING', '1')],
                    include_dirs = [kaldi_root + '/src', kaldi_root + '/tools/openfst/include', 'vosk', '/opt/intel/mkl/include'],
                    swig_opts=['-outdir', 'vosk', '-c++'],
                    libraries = kaldi_libraries,
                    extra_objects = lx,
                    sources = ['vosk/' + x for x in sources],
                    extra_link_args = kaldi_link_args,
                    extra_compile_args = ['-std=c++11', '-Wno-sign-compare', '-Wno-unused-variable', '-Wno-unused-local-typedefs', '-lmkl_rt'])

setuptools.setup(
    name="vosk", # Replace with your own username
    version="0.3.7",
    author="Alpha Cephei Inc",
    author_email="contact@alphacephei.com",
    description="API for Kaldi and Vosk",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/alphacep/vosk-api",
    packages=setuptools.find_packages(),
    ext_modules=[vosk_ext],
    cmdclass = {'build_py' : build_py},
    classifiers=[
        'Programming Language :: Python :: 3',
        'License :: OSI Approved :: Apache Software License',
        'Operating System :: Microsoft :: Windows',
        'Operating System :: POSIX :: Linux',
        'Operating System :: MacOS :: MacOS X',
        'Topic :: Software Development :: Libraries :: Python Modules'
    ],
    python_requires='>=3.5',
)

XISFgv Format
=============

This is a variant of the XISF format module, intended as a prototype for testing additional compression techniques. In the current state, it is not intended for merging into master!

The XISFgv-pxm.so module can be used in parallel to the standard XISF module, and registers the file extensions .XISFgv etc. . You can make it with linux/g++/makefile-x64-gv. 

The techniques used here are frequently used in scientific applications using a lot of numeric data, for instance as part of the HDF5 and netCDF file formats. Popular implementations are bitshuffle (as used here) and blosc. They are lossless compressions that achieve good compression rations with numeric data, because bit and/or bytes are re-arranged before doing the actual compression - increasing the degree of redundancy in numeric data. Float arrays, for instance, are otherwise difficult to compress and require a lot of work by the compression algorithm. In the rearranged version, the data is much easier to compress. Some are even recommending to do this kind of compression always, since the saved amount of I/O bandwidth more than compensates for the additional computation required.

Compression ratios are similar to, and sometimes higher, than what can be achieved with zip. But the real advantage is that the compression/decompression algorithms are simpler than zip, and much faster.

Currently I have implemented the following:
* Writing:
  * compressed with level 1-9 is identical to the original XISF format
  * level 10: Use bitshuffle, then zip level 9. We assume an element size of 4 (float, int, ..)
  * level 11: Use bitshuffle, then lz4 for compression
* Reading:
  * if compressed, assume file was written with level 11
  * the other levels are not supported

The current constraints of the module are caused by the following:
* This is a prototype for evaluating feasability and performance
* XISF is currently under heavy development. I think it is better to wait for stabilization, or
  implement the new compression methods as part of the stabilization
* I am not sure how this should be presented in the GUI. I dont want to add too many additional options
* In XISFInput/OutputDataBlock, I dont have information about
  * used compression method
  * size of element (=sizeof(numericDataTypeOfImage))
  * blockSize used during compression
  * I am also not sure if I can assume that the data is padded to lenght of 8 bytes as required
    by shuffle (did not want to dive too much into PCL sources)

The currently achieved sizes and compression times for a small set of test images is
as follows:

TestImage         |Milkyway|PI Benchmark|Planets|SuperBias
------------------|--------|------------|-------|----------
size FITS [MB]    |117     |13          |7.4    |39
size XISF [MB]    |117     |13          |7.4    |39
size level 9 [MB] |98      |11          |0.65   |23
time level 9 [s]  |9.0     |0.5         |0.34   |6.5
size level 10 [MB]|92      |9.4         |1.1    |18
time level 10 [s] |8.2     |0.84        |1.7    |1.3
size level 11 [MB]|95      |9.6         |1.6    |21
time level 11 [s] |0.28    |0.04        |0.029  |1.3

Umcompression for level 11 is always blindingly fast.

From my point of view, it seems to be a reasonable idea to include at least level 11 type compression
into XISF. Compression ratios are good, and it is always a lot faster than ZIP. Due to the extremely fast
operation, it is even an option to *always* use it. Direct access to specific data rows can still be
offered by always compressing chunks of the data (as already discussed elsewhere).

Enjoy,
Georg


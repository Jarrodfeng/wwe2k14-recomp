// fixture: invented code (big), a 70-line function for the size-cap case
namespace fixture {

int BigSum(const int* v) {
  int s = 0;
  s += v[1] * 2;  // step 1
  s += v[2] * 3;  // step 2
  s += v[3] * 4;  // step 3
  s += v[4] * 5;  // step 4
  s += v[5] * 6;  // step 5
  s += v[6] * 7;  // step 6
  s += v[7] * 1;  // step 7
  s += v[8] * 2;  // step 8
  s += v[9] * 3;  // step 9
  s += v[10] * 4;  // step 10
  s += v[11] * 5;  // step 11
  s += v[12] * 6;  // step 12
  s += v[13] * 7;  // step 13
  s += v[14] * 1;  // step 14
  s += v[15] * 2;  // step 15
  s += v[16] * 3;  // step 16
  s += v[17] * 4;  // step 17
  s += v[18] * 5;  // step 18
  s += v[19] * 6;  // step 19
  s += v[20] * 7;  // step 20
  s += v[21] * 1;  // step 21
  s += v[22] * 2;  // step 22
  s += v[23] * 3;  // step 23
  s += v[24] * 4;  // step 24
  s += v[25] * 5;  // step 25
  s += v[26] * 6;  // step 26
  s += v[27] * 7;  // step 27
  s += v[28] * 1;  // step 28
  s += v[29] * 2;  // step 29
  s += v[30] * 3;  // step 30
  s += v[31] * 4;  // step 31
  s += v[32] * 5;  // step 32
  s += v[33] * 6;  // step 33
  s += v[34] * 7;  // step 34
  s += v[35] * 1;  // step 35
  s += v[36] * 2;  // step 36
  s += v[37] * 3;  // step 37
  s += v[38] * 4;  // step 38
  s += v[39] * 5;  // step 39
  s += v[40] * 6;  // step 40
  s += v[41] * 7;  // step 41
  s += v[42] * 1;  // step 42
  s += v[43] * 2;  // step 43
  s += v[44] * 3;  // step 44
  s += v[45] * 4;  // step 45
  s += v[46] * 5;  // step 46
  s += v[47] * 6;  // step 47
  s += v[48] * 7;  // step 48
  s += v[49] * 1;  // step 49
  s += v[50] * 2;  // step 50
  s += v[51] * 3;  // step 51
  s += v[52] * 4;  // step 52
  s += v[53] * 5;  // step 53
  s += v[54] * 6;  // step 54
  s += v[55] * 7;  // step 55
  s += v[56] * 1;  // step 56
  s += v[57] * 2;  // step 57
  s += v[58] * 3;  // step 58
  s += v[59] * 4;  // step 59
  s += v[60] * 5;  // step 60
  s += v[61] * 6;  // step 61
  s += v[62] * 7;  // step 62
  return s;
}

}  // namespace fixture

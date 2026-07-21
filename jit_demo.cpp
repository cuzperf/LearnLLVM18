int gcd(int x, int y) {
  if (x < 0) x = -x;
  if (y < 0) y = -y;
  while (y) {
    int z = x % y;
    x = y;
    y = z;
  }
  return x;
}

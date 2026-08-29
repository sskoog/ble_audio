# Test pulse patterns for LED's
# Human eyes have a logarithmic sensitivity to light, 
# so controlling LED brighness with a linear scale does not look great.

import matplotlib.pyplot as plt
import numpy as np

# Time array of 1 second
t = np.linspace(0, 1, 256)

# Assume brightness scale is 0..1

# Linear slope (trangle wave)
l1 = np.array( [2*x if x < 0.5 else 2-2*x for x in t] )
# Quadratic curve
l2 = l1*l1
# Cubic curve
l3 = l1*l1*l1

# Sine brightness
b1 = np.sin(np.pi*t)
b2 = b1*b1        # Sin^2
b3 = b1*b1*b1     # Sin^3


plt.plot(t, l1, label="Linear")
plt.plot(t, l2, label="Quadratic")
plt.plot(t, l3, label="Cubic")
plt.plot(t, b1, label="Sine")
plt.plot(t, b2, label="Sin^2")
plt.plot(t, b3, label="Sin^3")
plt.grid(alpha=0.6)
plt.title("Brightness pulse pattern")
plt.ylabel("Brightness")
plt.xlabel("Time (s)")
plt.legend()
plt.show()

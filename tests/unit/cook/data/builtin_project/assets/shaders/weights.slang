// Imported by blur.slang.
module weights;

public float blur_weight(int i, int radius)
{
    return 1.0 / float(2 * radius + 1) + 0.0 * float(i);
}

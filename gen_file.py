import sys, argparse, random, string

def get_args(argv):
    parser = argparse.ArgumentParser(description='File generation tool')
    parser.add_argument('-f', '--filename', required=False, default='test_file.txt')
    parser.add_argument('-n', '--num-bytes-to-write', required=False, default=10000000, type=int)
    parser.add_argument('-b', '--binary-mode', action='store_true', help='Generate binary file instead of text')
    return parser.parse_args()

def generate_random_sentences(filename,bytes_to_write):
    import random
    nouns = ("puppy", "car", "rabbit", "girl", "monkey")
    verbs = ("runs", "hits", "jumps", "drives", "leaps")
    adv = ("crazily.", "dutifully.", "foolishly.", "merrily.", "occasionally.")

    all = [nouns, verbs, adv]

    bytes_written = 0
    with open(filename,'w') as f:
        while bytes_written < bytes_to_write:
            line = ' '.join([random.choice(i) for i in all]) + '\n'
            f.writelines(line)
            bytes_written += len(line)

def generate_random_binary_file(filename: str, num_bytes: int, chunk_size: int = 8192):
    """
    Generate a binary file with non-cryptographic random content (credit to chatGPT).

    :param filename: Name of the file to be created
    :param num_bytes: Total number of random bytes to write
    :param chunk_size: Number of bytes to write at once (default: 8KB)
    """
    with open(filename, 'wb') as f:
        bytes_written = 0
        while bytes_written < num_bytes:
            # Calculate how many bytes to write in this chunk
            to_write = min(chunk_size, num_bytes - bytes_written)

            # Generate a list of random bytes and convert to bytes
            chunk = bytes(random.getrandbits(8) for _ in range(to_write))

            f.write(chunk)
            bytes_written += to_write

def main(argv):
    args = get_args(argv)

    if args.binary_mode:
        generate_random_binary_file(args.filename, args.num_bytes_to_write)
    else:
        generate_random_sentences(args.filename, args.num_bytes_to_write)

if __name__ == "__main__":
    main(sys.argv[1:])

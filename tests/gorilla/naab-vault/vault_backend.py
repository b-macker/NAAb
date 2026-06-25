import argparse
import base64

def main():
    # Initialize the argument parser
    parser = argparse.ArgumentParser(description="Simple Base64 Encoder")

    # Add the --encrypt argument
    parser.add_argument(
        '--encrypt', 
        type=str, 
        help="The string you want to base64 encode"
    )

    args = parser.parse_args()

    if args.encrypt:
        # 1. Convert the string to bytes (base64 requires bytes-like objects)
        message_bytes = args.encrypt.encode('utf-8')
        
        # 2. Base64 encode the bytes
        base64_bytes = base64.b64encode(message_bytes)
        
        # 3. Decode the resulting bytes back into a string for printing
        base64_string = base64_bytes.decode('utf-8')
        
        print(base64_string)
    else:
        # If no argument is provided, print the help message
        parser.print_help()

if __name__ == "__main__":
    main()
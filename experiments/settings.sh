CORX_PATH="temp/"
CORRS_PATH="out/"
LOG_PATH="log/"
RAMDISK_SIZE=900M
# for sftp: UPLOAD_SERVER=sw@192.168.2.1
UPLOAD_SERVER=192.168.2.1:2121
RECEIVERS=192.168.2.130,192.168.2.133
NUM_RUNS=1

# ODROID_ID="A"
# PARAMS="--flagfile=flags.cfg --input=rtlsdr"

NOISE_GPIO=(
  238  # Pin 12
  236  # Pin 16
  233  # Pin 18
  231  # Pin 22
)

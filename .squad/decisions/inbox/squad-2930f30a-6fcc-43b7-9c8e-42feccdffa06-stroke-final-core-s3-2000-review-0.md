## Proposal from rai
Run: squad-2930f30a-6fcc-43b7-9c8e-42feccdffa06

建议批准主协调器在已配置的 16 MB CoreS3 上，从 package 根目录执行 `python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 --before default-reset --after hard-reset write-flash @flash_args`。

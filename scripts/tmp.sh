cd ~/lerning/project

# 1. 创建目标目录
mkdir -p ~/.local/bin

# 2. 移动 uv 和 uvx 到 bin 目录
mv uv-x86_64-unknown-linux-gnu/uv ~/.local/bin/
mv uv-x86_64-unknown-linux-gnu/uvx ~/.local/bin/

# 3. 删除空目录
rmdir uv-x86_64-unknown-linux-gnu/

# 4. 添加执行权限
chmod +x ~/.local/bin/uv ~/.local/bin/uvx

# 5. 添加到 PATH
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

# 6. 验证
uv --version

# 7. 安装 kimi-cli
uv tool install kimi-cli

# 在前面

我下载了VScode和Sublime3，但是却在“所有软件”里面找不到它们，也无法将它们加入到喜欢从而创建快捷方式，这样每次打开都很麻烦。

## 解决办法

在网上找到了解决方法

``` Shell
# 首先下载tar.gz文件
[root@Ubuntu:VScode]# https://vscode.cdn.azure.cn/stable/79b44aa704ce542d8ca4a3cc44cfca566e7720f1/code-stable-code_1.21.1-1521038896_amd64.tar.gz

# 解压缩
[root@Ubuntu:VScode]# tar -xzvf code-stable-code_1.21.1-1521038896_amd64.tar.gz

# 移动文件
[root@Ubuntu:VScode]# mv VSCode-linux-x64 /usr/local/

# 复制文件图标
[root@Ubuntu:VScode]# mv /usr/local/VSCode-linux-x64/resources/app/resources/linux/code.png  /usr/share/icons/

# 创建启动器
[root@Ubuntu:VScode]# vim /usr/share/applications/VScode.desktop
[Desktop Entry]
Name=Visual Studio Code
Comment=Multi-platform code editor for Linux
Exec=/usr/local/VSCode-linux-x64/code
Icon=/usr/share/icons/code.png
Type=Application
StartupNotify=true
Categories=TextEditor;Development;Utility;
MimeType=text/plain;

# 如果要在桌面上创建快捷方式
[ctom@Ubuntu:VScode]$ cp /usr/share/applications/VSCode.desktop  ~/Desktop/
[ctom#Ubuntu:VScode]$ chmod a+x ~/Desktop/VScode.desktop
```

## 网络链接

[创建VSCode图标](https://www.cnblogs.com/lzpong/p/6145511.html)
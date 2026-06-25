#pragma once

#include <QWidget>

// 「找母狗」一级页:一排快速跳转按钮,点击后在系统默认浏览器打开对应 URL。
// 按钮文字 / 跳转 URL / 字体都在 LinksPage.cpp 顶部的「手动填写区」里改。
class LinksPage : public QWidget {
    Q_OBJECT

public:
    explicit LinksPage(QWidget* parent = nullptr);
};

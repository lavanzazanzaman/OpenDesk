// The system check's rows (both platforms): a status icon, the check's name and what was found, in one card.
#pragma once

#include "setup.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

inline void fillSetupRows(QVBoxLayout * list, QLabel * summary, const QList<SetupItem> & items)
{
	while (QLayoutItem * item = list->takeAt(0))
	{
		if (QWidget * w = item->widget())
		{
			w->hide();
			w->deleteLater();
		}
		delete item;
	}
	int problems = 0;
	for (int i = 0; i < items.size(); ++i)
	{
		const SetupItem & item = items[i];
		problems += !item.ok;
		if (i > 0)
		{
			auto * line = new QFrame;
			line->setObjectName("rowSeparator");
			line->setFixedHeight(1);
			list->addWidget(line);
		}
		auto * row = new QWidget;
		auto * h = new QHBoxLayout(row);
		h->setContentsMargins(0, 9, 0, 9);
		h->setSpacing(12);
		auto * icon = new QLabel;
		icon->setFixedSize(20, 20);
		icon->setPixmap(QIcon(item.ok ? ":/assets/opendesk/status-ok.svg" : ":/assets/opendesk/status-warn.svg")
		                        .pixmap(QSize(20, 20), row->devicePixelRatioF()));
		auto * text = new QVBoxLayout;
		text->setSpacing(2);
		auto * title = new QLabel(item.title);
		title->setObjectName("settingTitle");
		title->setWordWrap(true);
		auto * detail = new QLabel(item.detail);
		detail->setObjectName(item.ok ? "settingDescription" : "settingWarning");
		detail->setWordWrap(true);
		detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
		text->addWidget(title);
		text->addWidget(detail);
		h->addWidget(icon, 0, Qt::AlignTop);
		h->addLayout(text, 1);
		list->addWidget(row);
	}
	if (summary)
	{
		summary->setText(problems ? QString("%1 of %2 checks need attention").arg(problems).arg(items.size())
		                          : QString("All %1 checks passed").arg(items.size()));
		summary->setObjectName(problems ? "summaryWarning" : "summaryOk");
		summary->style()->unpolish(summary);
		summary->style()->polish(summary);
	}
}

// the dialog's body: summary line, then the rows in a card that scrolls when the window is short
inline QVBoxLayout * makeSetupList(QVBoxLayout * dialog, QLabel *& summary)
{
	summary = new QLabel;
	dialog->addWidget(summary);
	auto * card = new QFrame;
	card->setObjectName("card");
	auto * list = new QVBoxLayout(card);
	list->setContentsMargins(16, 6, 16, 6);
	list->setSpacing(0);
	// a scroll area sizes wrapped labels by their height-for-width (a plain nested layout cut them to one line)
	auto * scroll = new QScrollArea;
	scroll->setObjectName("setupScroll");
	scroll->setWidget(card);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setMinimumHeight(600);
	dialog->addWidget(scroll, 1);
	return list;
}

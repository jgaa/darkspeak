#ifndef STRATEGY_H
#define STRATEGY_H

#include <QSqlTableModel>


namespace ds {
namespace models {

class Strategy {
public:
    Strategy(QSqlTableModel& model, QSqlTableModel::EditStrategy newStrategy)
        : model_{model}, old_strategy_{model.editStrategy()}
    {
        model_.setEditStrategy(newStrategy);
    }

    ~Strategy() {
        model_.setEditStrategy(old_strategy_);
    }

private:
    QSqlTableModel& model_;
    const QSqlTableModel::EditStrategy old_strategy_;
};

}} // namesspaces


#endif // STRATEGY_H
